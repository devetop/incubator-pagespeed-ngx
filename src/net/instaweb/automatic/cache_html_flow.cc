/*
 * Copyright 2013 Google Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Authors: mmohabey@google.com (Megha Mohabey)
//          pulkitg@google.com (Pulkit Goyal)

#include "net/instaweb/automatic/public/cache_html_flow.h"

#include "base/logging.h"
#include "net/instaweb/automatic/public/html_detector.h"
#include "net/instaweb/automatic/public/proxy_fetch.h"
#include "net/instaweb/http/http.pb.h"
#include "net/instaweb/http/public/async_fetch.h"
#include "net/instaweb/http/public/http_value.h"
#include "net/instaweb/http/public/log_record.h"
#include "net/instaweb/http/public/meta_data.h"
#include "net/instaweb/http/public/request_headers.h"
#include "net/instaweb/http/public/response_headers.h"
#include "net/instaweb/public/global_constants.h"
#include "net/instaweb/rewriter/cache_html_info.pb.h"
#include "net/instaweb/rewriter/public/blink_util.h"
#include "net/instaweb/rewriter/public/critical_images_finder.h"
#include "net/instaweb/rewriter/public/furious_matcher.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "net/instaweb/rewriter/public/static_asset_manager.h"
#include "net/instaweb/util/public/abstract_mutex.h"
#include "net/instaweb/util/public/basictypes.h"
#include "net/instaweb/util/public/function.h"
#include "net/instaweb/util/public/hasher.h"
#include "net/instaweb/util/public/property_cache.h"
#include "net/instaweb/util/public/proto_util.h"
#include "net/instaweb/util/public/statistics.h"
#include "net/instaweb/util/public/string_util.h"
#include "net/instaweb/util/public/thread_synchronizer.h"
#include "net/instaweb/util/public/thread_system.h"
#include "net/instaweb/util/public/timer.h"
#include "net/instaweb/util/public/scoped_ptr.h"

namespace net_instaweb {

const char kBlinkJsString[] =
    "<script type=\"text/javascript\" src=\"%s\"></script>";
const char kCacheHtmlSuffixJsString[] =
    "<script type=\"text/javascript\">"
    "pagespeed.panelLoaderInit();"
    "pagespeed.panelLoader.loadCriticalData({});"
    "pagespeed.panelLoader.loadImagesData({});"
    "</script>\n";

const char CacheHtmlFlow::kBackgroundComputationDone[] =
    "BackgroundComputation:Done";
const char CacheHtmlFlow::kNumCacheHtmlHits[] =
    "num_cache_html_hits";
const char CacheHtmlFlow::kNumCacheHtmlMisses[] =
    "num_cache_html_misses";
const char CacheHtmlFlow::kNumCacheHtmlMatches[] =
    "num_cache_html_matches";
const char CacheHtmlFlow::kNumCacheHtmlMismatches[] =
    "num_cache_html_mismatches";
const char CacheHtmlFlow::kNumCacheHtmlMismatchesCacheDeletes[] =
    "num_cache_html_mismatch_cache_deletes";
const char CacheHtmlFlow::kNumCacheHtmlSmartdiffMatches[] =
    "num_cache_html_smart_diff_matches";
const char CacheHtmlFlow::kNumCacheHtmlSmartdiffMismatches[] =
    "num_cache_html_smart_diff_mismatches";

namespace {

// Reads requisite info from Property Page. After reading, property page in
// driver is set to NULL, so that no one writes to property cache while
// rewriting cached html.
// TODO(mmohabey): Move the logic of copying properties in rewrite_driver when
// it is cloned.
void InitDriverWithPropertyCacheValues(
    RewriteDriver* cache_html_driver, PropertyPage* page) {
  cache_html_driver->set_unowned_property_page(page);
  // TODO(mmohabey): Critical line info should be populated here.

  // Populating critical images in cache html driver.
  CriticalImagesFinder* critical_images_finder =
      cache_html_driver->server_context()->critical_images_finder();
  if (critical_images_finder->IsMeaningful(cache_html_driver)) {
    critical_images_finder->UpdateCriticalImagesSetInDriver(cache_html_driver);
  }
  cache_html_driver->set_unowned_property_page(NULL);
}

class CacheHtmlComputationFetch : public AsyncFetch {
 public:
  CacheHtmlComputationFetch(const GoogleString& url,
                            RewriteDriver* rewrite_driver,
                            CacheHtmlInfo* cache_html_info)
      : AsyncFetch(rewrite_driver->request_context()),
        url_(url),
        server_context_(rewrite_driver->server_context()),
        options_(rewrite_driver->options()),
        rewrite_driver_(rewrite_driver),
        cache_html_info_(cache_html_info),
        claims_html_(false),
        probable_html_(false),
        content_length_over_threshold_(false),
        non_ok_status_code_(false),
        cache_html_change_mutex_(
            server_context_->thread_system()->NewMutex()),
        finish_(false) {
    // Makes rewrite_driver live longer as ProxyFetch may called Cleanup()
    // on the rewrite_driver even if ComputeCacheHtmlInfo() has not yet
    // been triggered.
    rewrite_driver_->increment_async_events_count();
    Statistics* stats = server_context_->statistics();
    num_cache_html_misses_ = stats->GetTimedVariable(
        CacheHtmlFlow::kNumCacheHtmlMisses);
    num_cache_html_matches_ = stats->GetTimedVariable(
        CacheHtmlFlow::kNumCacheHtmlMatches);
    num_cache_html_mismatches_ = stats->GetTimedVariable(
        CacheHtmlFlow::kNumCacheHtmlMismatches);
    num_cache_html_mismatches_cache_deletes_ = stats->GetTimedVariable(
        CacheHtmlFlow::kNumCacheHtmlMismatchesCacheDeletes);
    num_cache_html_smart_diff_matches_ = stats->GetTimedVariable(
        CacheHtmlFlow::kNumCacheHtmlSmartdiffMatches);
    num_cache_html_smart_diff_mismatches_ = stats->GetTimedVariable(
        CacheHtmlFlow::kNumCacheHtmlSmartdiffMismatches);
  }

  virtual ~CacheHtmlComputationFetch() {
    rewrite_driver_->decrement_async_events_count();
    ThreadSynchronizer* sync = server_context_->thread_synchronizer();
    sync->Signal(CacheHtmlFlow::kBackgroundComputationDone);
  }

  virtual void HandleHeadersComplete() {
    if (response_headers()->status_code() == HttpStatus::kOK) {
      claims_html_ = response_headers()->IsHtmlLike();
      int64 content_length;
      bool content_length_found = response_headers()->FindContentLength(
          &content_length);
      if (content_length_found && content_length >
          options_->blink_max_html_size_rewritable()) {
        content_length_over_threshold_ = true;
      }
    } else {
      non_ok_status_code_ = true;
      VLOG(1) << "Non 200 response code for: " << url_;
    }
  }

  virtual bool HandleWrite(const StringPiece& content,
                           MessageHandler* handler) {
    if (!claims_html_ || content_length_over_threshold_) {
      return true;
    }
    if (!html_detector_.already_decided() &&
        html_detector_.ConsiderInput(content)) {
      if (html_detector_.probable_html()) {
        probable_html_ = true;
        html_detector_.ReleaseBuffered(&buffer_);
      }
    }
    // TODO(poojatandon): share this logic of finding the length and setting a
    // limit with http_cache code.
    if (probable_html_) {
      if (unsigned(buffer_.size() + content.size()) >
          options_->blink_max_html_size_rewritable()) {
        content_length_over_threshold_ = true;
        buffer_.clear();
      } else {
        content.AppendToString(&buffer_);
      }
    }
    return true;
  }

  virtual bool HandleFlush(MessageHandler* handler) {
    return true;
    // No operation.
  }

  virtual void HandleDone(bool success) {
    if (non_ok_status_code_ || !success || !claims_html_ || !probable_html_ ||
        content_length_over_threshold_) {
      if (cache_html_info_->has_cached_html()) {
        // This means it is a cache hit case.  Currently it also means diff is
        // enabled (possibly in logging mode), since CacheHtmlComputationFetch
        // is attached in cache hit case only when diff is enabled.
        // Calling Finish since the deletion of this object needs to be
        // synchronized with HandleDone call in AsyncFetchWithHeadersInhibited,
        // since that class refers to this object.
        Finish();
      } else {
        delete this;
      }
      return;
    }
    if (rewrite_driver_->options()->enable_blink_html_change_detection() ||
        rewrite_driver_->options()->
        enable_blink_html_change_detection_logging()) {
      // We do diff mismatch detection in cache miss case also so that we can
      // update the content hash and smart text hash in CacheHtmlInfo in pcache.
      CreateHtmlChangeDetectionDriverAndRewrite();
    } else {
      CreateCacheHtmlComputationDriverAndRewrite();
    }
  }

  void CreateHtmlChangeDetectionDriverAndRewrite() {
    LOG(INFO) << "CreateHtmlChangeDetectionDriverAndRewrite";
    RewriteOptions* options = rewrite_driver_->options()->Clone();
    options->ClearFilters();
    options->ForceEnableFilter(RewriteOptions::kRemoveComments);
    options->ForceEnableFilter(RewriteOptions::kStripNonCacheable);
    options->ForceEnableFilter(RewriteOptions::kComputeVisibleText);
    server_context_->ComputeSignature(options);
    html_change_detection_driver_ =
        server_context_->NewCustomRewriteDriver(options, request_context());
    value_.Clear();
    html_change_detection_driver_->SetWriter(&value_);
    html_change_detection_driver_->set_response_headers_ptr(response_headers());
    complete_finish_parse_html_change_driver_fn_ = MakeFunction(
        this,
        &CacheHtmlComputationFetch::CompleteFinishParseForHtmlChangeDriver);
    html_change_detection_driver_->AddLowPriorityRewriteTask(
        MakeFunction(
            this, &CacheHtmlComputationFetch::Parse,
            &CacheHtmlComputationFetch::CancelParseForHtmlChangeDriver,
            html_change_detection_driver_,
            complete_finish_parse_html_change_driver_fn_));
  }

  void CreateCacheHtmlComputationDriverAndRewrite() {
    LOG(INFO) << "CreateCacheHtmlComputationDriverAndRewrite";
    RewriteOptions* options = rewrite_driver_->options()->Clone();
    options->ClearFilters();
    options->ForceEnableFilter(RewriteOptions::kStripNonCacheable);
    cache_html_computation_driver_ =
        server_context_->NewCustomRewriteDriver(options, request_context());
    value_.Clear();
    cache_html_computation_driver_->SetWriter(&value_);
    cache_html_computation_driver_->set_response_headers_ptr(
        response_headers());
    complete_finish_parse_cache_html_driver_fn_ = MakeFunction(
        this, &CacheHtmlComputationFetch::
        CompleteFinishParseForCacheHtmlComputationDriver);
    cache_html_computation_driver_->AddLowPriorityRewriteTask(
        MakeFunction(
            this, &CacheHtmlComputationFetch::Parse,
            &CacheHtmlComputationFetch::
            CancelParseForCacheHtmlComputationDriver,
            cache_html_computation_driver_,
            complete_finish_parse_cache_html_driver_fn_));
  }

  void Parse(RewriteDriver* driver, Function* task) {
    driver->StartParse(url_);
    driver->ParseText(buffer_);
    driver->FinishParseAsync(task);
  }

  void CancelParseForCacheHtmlComputationDriver(RewriteDriver* driver,
                                                   Function* task) {
    LOG(WARNING) << "Cache Html computation dropped due to load"
                 << " for url: " << url_;
    complete_finish_parse_cache_html_driver_fn_->CallCancel();
    cache_html_computation_driver_->Cleanup();
    delete this;
  }

  void CancelParseForHtmlChangeDriver(RewriteDriver* driver, Function* task) {
    LOG(WARNING) << "Html change diff dropped due to load"
                 << " for url: " << url_;
    complete_finish_parse_html_change_driver_fn_->CallCancel();
    html_change_detection_driver_->Cleanup();
    Finish();
  }

  void CompleteFinishParseForCacheHtmlComputationDriver() {
    LOG(INFO) << "CompleteFinishParseForCacheHtmlComputationDriver";
    StringPiece rewritten_content;
    value_.ExtractContents(&rewritten_content);
    cache_html_info_->set_cached_html(rewritten_content.data(),
                                      rewritten_content.size());
    cache_html_info_->set_last_cached_html_computation_timestamp_ms(
        server_context_->timer()->NowMs());
    if (!cache_html_info_->cached_html().empty() &&
        !content_length_over_threshold_) {
      UpdatePropertyCacheWithCacheHtmlInfo();
    }
    delete this;
  }

  void CompleteFinishParseForHtmlChangeDriver() {
    LOG(INFO) << "CompleteFinishParseForHtmlChangeDriver";
    StringPiece output;
    value_.ExtractContents(&output);
    StringPieceVector result;
    net_instaweb::SplitStringUsingSubstr(
        output, BlinkUtil::kComputeVisibleTextFilterOutputEndMarker,
        &result);
    if (result.size() == 2) {
      computed_hash_smart_diff_ = server_context_->hasher()->Hash(result[0]);
      computed_hash_ = server_context_->hasher()->Hash(result[1]);
    }
    if (!cache_html_info_->has_cached_html()) {
      CreateCacheHtmlComputationDriverAndRewrite();
      return;
    }
    if (computed_hash_ != cache_html_info_->hash()) {
      num_cache_html_mismatches_->IncBy(1);
    } else {
      num_cache_html_matches_->IncBy(1);
    }
    if (computed_hash_smart_diff_ !=
        cache_html_info_->hash_smart_diff()) {
      num_cache_html_smart_diff_mismatches_->IncBy(1);
    } else {
      num_cache_html_smart_diff_matches_->IncBy(1);
    }
    Finish();
  }

  // This function should only be called if change detection is enabled and
  // this is a cache hit case. In such cases, the content may need to be deleted
  // from the property cache if a change was detected. This deletion should wait
  // for AsyncFetchWithHeadersInhibited to complete (HandleDone called) to
  // ensure that we do not delete entry from cache while it is still being used
  // to process the request.
  //
  // This method achieves this goal using a mutex protected
  // variable finish_. Both CacheHtmlComputationFetch and
  // AsyncFetchWithHeadersInhibited call this method once their processing is
  // done. The first call sets the value of finish_ to true and returns.
  // The second call to this method actually calls ProcessDiffResult.
  void Finish() {
    {
      ScopedMutex lock(cache_html_change_mutex_.get());
      if (!finish_) {
        finish_ = true;
        return;
      }
    }
    ProcessDiffResult();
  }

  // This method processes the result of html change detection. If a mismatch
  // is found, we delete the entry from the cache and trigger a cache html info
  // computation.
  void ProcessDiffResult() {
    LOG(INFO) << "ProcessDiffResult";
    if (computed_hash_.empty()) {
      LOG(WARNING) << "Computed hash is empty for url " << url_;
      delete this;
      return;
    }
    bool compute_cache_html_info = false;
    if (options_->use_smart_diff_in_blink()) {
      compute_cache_html_info =
          (computed_hash_smart_diff_ !=
              cache_html_info_->hash_smart_diff());
      LOG(INFO) << computed_hash_smart_diff_;
      LOG(INFO) << cache_html_info_->hash_smart_diff();
    } else {
      compute_cache_html_info =
          (computed_hash_ !=
              cache_html_info_->hash());
      LOG(INFO) << computed_hash_;
      LOG(INFO) << cache_html_info_->hash();
    }
    // TODO(mmohabey): Incorporate DiffInfo.

    if (options_->enable_blink_html_change_detection() &&
        compute_cache_html_info) {
      // TODO(mmohabey): Do not call delete here as we will be subsequently
      // updating the new value in property cache using
      // CreateCacheHtmlComputationDriverAndRewrite.
      DeleteCacheHtmlInfoFromPropertyCache();
      CreateCacheHtmlComputationDriverAndRewrite();
    } else if (options_->enable_blink_html_change_detection() ||
               computed_hash_ != cache_html_info_->hash() ||
               computed_hash_smart_diff_ !=
               cache_html_info_->hash_smart_diff()) {
      UpdatePropertyCacheWithCacheHtmlInfo();
      delete this;
    } else {
      delete this;
    }
  }

  void UpdatePropertyCacheWithCacheHtmlInfo() {
    LOG(INFO) << "Updating property cache";
    cache_html_info_->set_charset(response_headers()->DetermineCharset());
    cache_html_info_->set_hash(computed_hash_);
    cache_html_info_->set_hash_smart_diff(computed_hash_smart_diff_);

    LOG(INFO) << cache_html_info_->cached_html().size();
    LOG(INFO) << cache_html_info_->hash();
    LOG(INFO) << cache_html_info_->hash_smart_diff();
    PropertyCache* property_cache =
        rewrite_driver_->server_context()->page_property_cache();
    PropertyPage* page = rewrite_driver_->property_page();
    const PropertyCache::Cohort* cohort = property_cache->GetCohort(
        BlinkUtil::kBlinkCohort);
    GoogleString buf;
    cache_html_info_->SerializeToString(&buf);
    PropertyValue* property_value = page->GetProperty(
        cohort, BlinkUtil::kCacheHtmlRewriterInfo);
    property_cache->UpdateValue(buf, property_value);
    property_cache->WriteCohort(cohort, page);
  }

  void DeleteCacheHtmlInfoFromPropertyCache() {
    num_cache_html_mismatches_cache_deletes_->IncBy(1);
    const PropertyCache::Cohort* cohort =
        rewrite_driver_->server_context()->page_property_cache()->
            GetCohort(BlinkUtil::kBlinkCohort);
    PropertyPage* page = rewrite_driver_->property_page();
    page->DeleteProperty(cohort, BlinkUtil::kCacheHtmlRewriterInfo);
    // TODO(mmohabey): Call WriteCohort only once in
    // UpdatePropertyCacheWithCacheHtmlInfo and not here. This is to avoid
    // property cache write race.
    rewrite_driver_->server_context()->
        page_property_cache()->WriteCohort(cohort, page);
    // TODO(mmohabey): Add logic to propogate the deletes and deleting the
    // critical line info.
  }

 private:
  GoogleString url_;
  ServerContext* server_context_;
  const RewriteOptions* options_;
  GoogleString buffer_;
  HTTPValue value_;
  HtmlDetector html_detector_;
  GoogleString computed_hash_;
  GoogleString computed_hash_smart_diff_;
  HttpResponseHeaders http_response_headers_;

  // RewriteDriver passed to ProxyFetch to serve user-facing request.
  RewriteDriver* rewrite_driver_;
  // RewriteDriver used to parse the buffered html content.
  RewriteDriver* cache_html_computation_driver_;
  RewriteDriver* html_change_detection_driver_;
  scoped_ptr<LogRecord> log_record_;
  scoped_ptr<CacheHtmlInfo> cache_html_info_;
  Function* complete_finish_parse_cache_html_driver_fn_;
  Function* complete_finish_parse_html_change_driver_fn_;
  bool claims_html_;
  bool probable_html_;
  bool content_length_over_threshold_;
  bool non_ok_status_code_;

  // Variables to manage change detection processing.
  // Mutex
  scoped_ptr<AbstractMutex> cache_html_change_mutex_;
  bool finish_;  // protected by cache_html_change_mutex_

  TimedVariable* num_cache_html_misses_;
  TimedVariable* num_cache_html_matches_;
  TimedVariable* num_cache_html_mismatches_;
  TimedVariable* num_cache_html_mismatches_cache_deletes_;
  TimedVariable* num_cache_html_smart_diff_matches_;
  TimedVariable* num_cache_html_smart_diff_mismatches_;

  DISALLOW_COPY_AND_ASSIGN(CacheHtmlComputationFetch);
};

// AsyncFetch that doesn't call HeadersComplete() on the base fetch. Note that
// this class only links the request headers from the base fetch and does not
// link the response headers.
// This is used as a wrapper around the base fetch when CacheHtmlInfo is
// found in cache. This is done because the response headers and the
// cached html have been already been flushed out in the base fetch
// and we don't want to call HeadersComplete() twice on the base fetch.
// This class deletes itself when HandleDone() is called.
class AsyncFetchWithHeadersInhibited : public AsyncFetchUsingWriter {
 public:
  AsyncFetchWithHeadersInhibited(
      AsyncFetch* fetch,
      CacheHtmlComputationFetch* cache_html_computation_fetch)
      : AsyncFetchUsingWriter(fetch->request_context(), fetch),
        base_fetch_(fetch),
        cache_html_computation_fetch_(cache_html_computation_fetch) {
    set_request_headers(fetch->request_headers());
  }

 private:
  virtual ~AsyncFetchWithHeadersInhibited() {
  }

  virtual void HandleHeadersComplete() {}

  virtual void HandleDone(bool success) {
    base_fetch_->Done(success);
    if (cache_html_computation_fetch_ != NULL) {
      cache_html_computation_fetch_->Finish();
    }
    delete this;
  }

  AsyncFetch* base_fetch_;
  CacheHtmlComputationFetch* cache_html_computation_fetch_;

  DISALLOW_COPY_AND_ASSIGN(AsyncFetchWithHeadersInhibited);
};

}  // namespace

void CacheHtmlFlow::Start(
    const GoogleString& url,
    AsyncFetch* base_fetch,
    RewriteDriver* driver,
    ProxyFetchFactory* factory,
    ProxyFetchPropertyCallbackCollector* property_cache_callback) {
  LOG(INFO) << "Cache Html Flow Start:" << url;
  CacheHtmlFlow* flow = new CacheHtmlFlow(
      url, base_fetch, driver, factory, property_cache_callback);

  Function* func = MakeFunction(flow, &CacheHtmlFlow::CacheHtmlLookupDone,
                                &CacheHtmlFlow::Cancel);
  property_cache_callback->AddPostLookupTask(func);
}

void CacheHtmlFlow::InitStats(Statistics* stats) {
  stats->AddTimedVariable(kNumCacheHtmlHits,
                          ServerContext::kStatisticsGroup);
  stats->AddTimedVariable(kNumCacheHtmlMisses,
                          ServerContext::kStatisticsGroup);
  stats->AddTimedVariable(kNumCacheHtmlMatches,
                          ServerContext::kStatisticsGroup);
  stats->AddTimedVariable(kNumCacheHtmlMismatches,
                          ServerContext::kStatisticsGroup);
  stats->AddTimedVariable(kNumCacheHtmlMismatchesCacheDeletes,
                          ServerContext::kStatisticsGroup);
  stats->AddTimedVariable(kNumCacheHtmlSmartdiffMatches,
                          ServerContext::kStatisticsGroup);
  stats->AddTimedVariable(kNumCacheHtmlSmartdiffMismatches,
                          ServerContext::kStatisticsGroup);
}

CacheHtmlFlow::CacheHtmlFlow(
    const GoogleString& url,
    AsyncFetch* base_fetch,
    RewriteDriver* driver,
    ProxyFetchFactory* factory,
    ProxyFetchPropertyCallbackCollector* property_cache_callback)
    : url_(url),
      google_url_(url),
      base_fetch_(base_fetch),
      rewrite_driver_(driver),
      options_(driver->options()),
      factory_(factory),
      server_context_(driver->server_context()),
      property_cache_callback_(property_cache_callback),
      handler_(rewrite_driver_->server_context()->message_handler()) {
  Statistics* stats = server_context_->statistics();
  num_cache_html_misses_ = stats->GetTimedVariable(
      kNumCacheHtmlMisses);
  num_cache_html_hits_ = stats->GetTimedVariable(
      kNumCacheHtmlHits);
}

CacheHtmlFlow::~CacheHtmlFlow() {
}

void CacheHtmlFlow::CacheHtmlLookupDone() {
  LOG(INFO) << "CacheHtmlLookupDone:" << url_;
  PropertyPage* page = property_cache_callback_->
      GetPropertyPageWithoutOwnership(
          ProxyFetchPropertyCallback::kPagePropertyCache);
  PopulateCacheHtmlInfo(page);

  // TODO(mmohabey): Add timings and dashboard.
  if (cache_html_info_.has_cached_html()) {
    CacheHtmlHit(page);
  } else {
    CacheHtmlMiss();
  }
}

void CacheHtmlFlow::CacheHtmlMiss() {
  LOG(INFO) << "CacheHtmlMiss:" << url_;
  num_cache_html_misses_->IncBy(1);
  TriggerProxyFetch();
}

void CacheHtmlFlow::CacheHtmlHit(PropertyPage* page) {
  LOG(INFO) << "CacheHtmlHit:" << url_;
  num_cache_html_hits_->IncBy(1);
  StringPiece cached_html = cache_html_info_.cached_html();
  // TODO(mmohabey): Handle malformed html case.

  ResponseHeaders* response_headers = base_fetch_->response_headers();
  response_headers->SetStatusAndReason(HttpStatus::kOK);
  // TODO(pulkitg): Store content type in pcache.
  // TODO(mmohabey): Handle Meta tags.
  GoogleString content_type = StrCat(
      "text/html", cache_html_info_.has_charset() ?
      StrCat("; charset=", cache_html_info_.charset()) : "");
  response_headers->Add(HttpAttributes::kContentType, content_type);
  response_headers->Add(
      kPsaRewriterHeader,
      RewriteOptions::FilterId(RewriteOptions::kCacheHtml));
  response_headers->ComputeCaching();
  response_headers->SetDateAndCaching(server_context_->timer()->NowMs(), 0,
                                      ", private, no-cache");
  // If relevant, add the Set-Cookie header for furious experiments.
  if (options_->need_to_store_experiment_data() &&
      options_->running_furious()) {
    int furious_value = options_->furious_id();
    server_context_->furious_matcher()->StoreExperimentData(
        furious_value, url_,
        server_context_->timer()->NowMs() +
            options_->furious_cookie_duration_ms(),
        response_headers);
  }
  base_fetch_->HeadersComplete();

  // Clone the RewriteDriver which is used to rewrite the HTML that we are
  // trying to flush early.
  LOG(INFO) << "old" << rewrite_driver_;
  RewriteDriver* new_driver = rewrite_driver_->Clone();
  LOG(INFO) << "new" << new_driver;
  new_driver->set_response_headers_ptr(base_fetch_->response_headers());
  new_driver->set_flushing_cached_html(true);
  new_driver->SetWriter(base_fetch_);
  new_driver->StartParse(url_);

  InitDriverWithPropertyCacheValues(new_driver, page);

  new_driver->ParseText(cached_html);
  new_driver->FinishParseAsync(
      MakeFunction(this, &CacheHtmlFlow::CacheHtmlRewriteDone));
}

void CacheHtmlFlow::CacheHtmlRewriteDone() {
  rewrite_driver_->set_flushed_cached_html(true);

  StaticAssetManager* static_asset_manager =
      server_context_->static_asset_manager();
  base_fetch_->Write(StringPrintf(kBlinkJsString,
      static_asset_manager->GetAssetUrl(
          StaticAssetManager::kBlinkJs, options_).c_str()), handler_);
  base_fetch_->Write(kCacheHtmlSuffixJsString, handler_);
  base_fetch_->Flush(handler_);
  TriggerProxyFetch();
}

void CacheHtmlFlow::TriggerProxyFetch() {
  LOG(INFO) << "ProxyFetchTriggered:" << url_;
  bool flushed_cached_html = rewrite_driver_->flushed_cached_html();
  AsyncFetch* fetch = NULL;
  CacheHtmlComputationFetch* cache_html_computation_fetch = NULL;

  // Remove any headers that can lead to a 304, since CacheHtmlFlow can't
  // handle 304s.
  base_fetch_->request_headers()->RemoveAll(HttpAttributes::kIfNoneMatch);
  base_fetch_->request_headers()->RemoveAll(HttpAttributes::kIfModifiedSince);

  if (!flushed_cached_html || options_->enable_blink_html_change_detection() ||
      options_->enable_blink_html_change_detection_logging()) {
    CacheHtmlInfo* cache_html_info = new CacheHtmlInfo();
    cache_html_info->CopyFrom(cache_html_info_);
    cache_html_computation_fetch = new CacheHtmlComputationFetch(
        url_, rewrite_driver_, cache_html_info);
    // TODO(mmohabey) : Set a fixed user agent for fetching content from the
    // origin server if options->use_fixed_user_agent_for_blink_cache_misses()
    // is enabled.
  }

  if (flushed_cached_html) {
    // TODO(mmohabey): Disable LazyloadImages filter for the driver sending non
    // cacheables.
    fetch = new AsyncFetchWithHeadersInhibited(base_fetch_,
                                               cache_html_computation_fetch);
  } else {
    // PassThrough case.
    // This flow has side effect that DeferJs is applied in passthrough case
    // even when it is not explicitly enabled since it is added in
    // RewriteDriver::AddPostRenderFilters() if RewriteOptions::kCacheHtml is
    // enabled.
    fetch = base_fetch_;
  }

  factory_->StartNewProxyFetch(
            url_, fetch, rewrite_driver_, property_cache_callback_,
            cache_html_computation_fetch);
  delete this;
}

// TODO(mmohabey): Disable conflicting filters for cache html flow.

void CacheHtmlFlow::Cancel() {
  delete this;
}

void CacheHtmlFlow::PopulateCacheHtmlInfo(PropertyPage* page) {
  const PropertyCache::Cohort* cohort = server_context_->page_property_cache()->
      GetCohort(BlinkUtil::kBlinkCohort);
  if (page == NULL || cohort == NULL) {
    return;
  }

  PropertyValue* property_value = page->GetProperty(
      cohort, BlinkUtil::kCacheHtmlRewriterInfo);
  if (!property_value->has_value()) {
    return;
  }
  ArrayInputStream value(property_value->value().data(),
                         property_value->value().size());
  if (!cache_html_info_.ParseFromZeroCopyStream(&value)) {
    LOG(DFATAL) << "Parsing value from cache into CacheHtmlInfo failed.";
    cache_html_info_.Clear();
    return;
  }
  int64 expiration_time_ms =
      cache_html_info_.last_cached_html_computation_timestamp_ms() +
      options_->GetBlinkCacheTimeFor(google_url_);

  if (!options_->enable_blink_html_change_detection() &&
      server_context_->timer()->NowMs() > expiration_time_ms) {
    cache_html_info_.Clear();
    return;
  }
}

}  // namespace net_instaweb