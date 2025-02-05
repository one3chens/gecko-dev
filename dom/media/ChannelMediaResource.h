/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_media_ChannelMediaResource_h
#define mozilla_dom_media_ChannelMediaResource_h

#include "MediaResource.h"

namespace mozilla {

/**
 * This is the MediaResource implementation that wraps Necko channels.
 * Much of its functionality is actually delegated to MediaCache via
 * an underlying MediaCacheStream.
 *
 * All synchronization is performed by MediaCacheStream; all off-main-
 * thread operations are delegated directly to that object.
 */
class ChannelMediaResource : public BaseMediaResource
{
public:
  ChannelMediaResource(MediaResourceCallback* aDecoder,
                       nsIChannel* aChannel,
                       nsIURI* aURI,
                       bool aIsPrivateBrowsing);
  ChannelMediaResource(MediaResourceCallback* aDecoder,
                       nsIChannel* aChannel,
                       nsIURI* aURI,
                       const MediaChannelStatistics& aStatistics);
  ~ChannelMediaResource();

  // These are called on the main thread by MediaCache. These must
  // not block or grab locks, because the media cache is holding its lock.
  // Notify that data is available from the cache. This can happen even
  // if this stream didn't read any data, since another stream might have
  // received data for the same resource.
  void CacheClientNotifyDataReceived();
  // Notify that we reached the end of the stream. This can happen even
  // if this stream didn't read any data, since another stream might have
  // received data for the same resource.
  void CacheClientNotifyDataEnded(nsresult aStatus);
  // Notify that the principal for the cached resource changed.
  void CacheClientNotifyPrincipalChanged();
  // Notify the decoder that the cache suspended status changed.
  void CacheClientNotifySuspendedStatusChanged();

  // These are called on the main thread by MediaCache. These shouldn't block,
  // but they may grab locks --- the media cache is not holding its lock
  // when these are called.
  // Start a new load at the given aOffset. The old load is cancelled
  // and no more data from the old load will be notified via
  // MediaCacheStream::NotifyDataReceived/Ended.
  // This can fail.
  nsresult CacheClientSeek(int64_t aOffset, bool aResume);
  // Suspend the current load since data is currently not wanted
  nsresult CacheClientSuspend();
  // Resume the current load since data is wanted again
  nsresult CacheClientResume();

  bool IsSuspended();

  void ThrottleReadahead(bool bThrottle) override;

  // Main thread
  nsresult Open(nsIStreamListener** aStreamListener) override;
  nsresult Close() override;
  void     Suspend(bool aCloseImmediately) override;
  void     Resume() override;
  already_AddRefed<nsIPrincipal> GetCurrentPrincipal() override;
  bool     CanClone() override;
  already_AddRefed<BaseMediaResource> CloneData(
    MediaResourceCallback* aDecoder) override;
  nsresult ReadFromCache(char* aBuffer, int64_t aOffset, uint32_t aCount) override;

  // Other thread
  void     SetReadMode(MediaCacheStream::ReadMode aMode) override;
  void     SetPlaybackRate(uint32_t aBytesPerSecond) override;
  nsresult ReadAt(int64_t offset, char* aBuffer,
                  uint32_t aCount, uint32_t* aBytes) override;
  // Data stored in IO&lock-encumbered MediaCacheStream, caching recommended.
  bool ShouldCacheReads() override { return true; }
  int64_t Tell() override;

  // Any thread
  void    Pin() override;
  void    Unpin() override;
  double  GetDownloadRate(bool* aIsReliable) override;
  int64_t GetLength() override;
  int64_t GetNextCachedData(int64_t aOffset) override;
  int64_t GetCachedDataEnd(int64_t aOffset) override;
  bool    IsDataCachedToEndOfResource(int64_t aOffset) override;
  bool    IsTransportSeekable() override;

  size_t SizeOfExcludingThis(MallocSizeOf aMallocSizeOf) const override {
    // Might be useful to track in the future:
    //   - mListener (seems minor)
    //   - mChannelStatistics (seems minor)
    size_t size = BaseMediaResource::SizeOfExcludingThis(aMallocSizeOf);
    size += mCacheStream.SizeOfExcludingThis(aMallocSizeOf);

    return size;
  }

  size_t SizeOfIncludingThis(MallocSizeOf aMallocSizeOf) const override {
    return aMallocSizeOf(this) + SizeOfExcludingThis(aMallocSizeOf);
  }

  class Listener final
    : public nsIStreamListener
    , public nsIInterfaceRequestor
    , public nsIChannelEventSink
    , public nsIThreadRetargetableStreamListener
  {
    ~Listener() {}
  public:
    Listener(ChannelMediaResource* aResource, int64_t aOffset)
      : mResource(aResource)
      , mOffset(aOffset)
    {}

    NS_DECL_ISUPPORTS
    NS_DECL_NSIREQUESTOBSERVER
    NS_DECL_NSISTREAMLISTENER
    NS_DECL_NSICHANNELEVENTSINK
    NS_DECL_NSIINTERFACEREQUESTOR
    NS_DECL_NSITHREADRETARGETABLESTREAMLISTENER

    void Revoke() { mResource = nullptr; }

  private:
    RefPtr<ChannelMediaResource> mResource;
    const int64_t mOffset;
  };
  friend class Listener;

  nsresult GetCachedRanges(MediaByteRangeSet& aRanges) override;

protected:
  bool IsSuspendedByCache();
  // These are called on the main thread by Listener.
  nsresult OnStartRequest(nsIRequest* aRequest, int64_t aRequestOffset);
  nsresult OnStopRequest(nsIRequest* aRequest, nsresult aStatus);
  nsresult OnDataAvailable(nsIRequest* aRequest,
                           nsIInputStream* aStream,
                           uint32_t aCount);
  nsresult OnChannelRedirect(nsIChannel* aOld,
                             nsIChannel* aNew,
                             uint32_t aFlags,
                             int64_t aOffset);

  // Opens the channel, using an HTTP byte range request to start at aOffset
  // if possible. Main thread only.
  nsresult OpenChannel(int64_t aOffset);
  nsresult RecreateChannel();
  // Add headers to HTTP request. Main thread only.
  nsresult SetupChannelHeaders(int64_t aOffset);
  // Closes the channel. Main thread only.
  void CloseChannel();
  // Update the principal for the resource. Main thread only.
  void UpdatePrincipal();

  int64_t GetOffset() const;

  // Parses 'Content-Range' header and returns results via parameters.
  // Returns error if header is not available, values are not parse-able or
  // values are out of range.
  nsresult ParseContentRangeHeader(nsIHttpChannel * aHttpChan,
                                   int64_t& aRangeStart,
                                   int64_t& aRangeEnd,
                                   int64_t& aRangeTotal);

  static nsresult CopySegmentToCache(nsIInputStream* aInStream,
                                     void* aResource,
                                     const char* aFromSegment,
                                     uint32_t aToOffset,
                                     uint32_t aCount,
                                     uint32_t* aWriteCount);

  nsresult CopySegmentToCache(const char* aFromSegment,
                              uint32_t aCount,
                              uint32_t* aWriteCount);

  // Main thread access only
  RefPtr<Listener> mListener;
  // When this flag is set, if we get a network error we should silently
  // reopen the stream.
  bool               mReopenOnError;

  // Any thread access
  MediaCacheStream mCacheStream;

  MediaChannelStatistics mChannelStatistics;
  ChannelSuspendAgent mSuspendAgent;
};


} // namespace mozilla

#endif // mozilla_dom_media_ChannelMediaResource_h
