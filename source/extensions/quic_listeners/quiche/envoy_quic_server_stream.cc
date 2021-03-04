#include "extensions/quic_listeners/quiche/envoy_quic_server_stream.h"

#include <openssl/bio.h>
#include <openssl/evp.h>

#include <memory>
#include <stdexcept>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif

#include "quiche/quic/core/http/quic_header_list.h"
#include "quiche/quic/core/quic_session.h"
#include "quiche/spdy/core/spdy_header_block.h"
#include "extensions/quic_listeners/quiche/platform/quic_mem_slice_span_impl.h"
#include "quiche/common/platform/api/quiche_text_utils.h"

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "absl/strings/str_split.h"
#include "extensions/quic_listeners/quiche/envoy_quic_server_session.h"

#include "common/buffer/buffer_impl.h"
#include "common/http/header_map_impl.h"
#include "common/common/assert.h"
#include "common/http/header_utility.h"

#include "server/backtrace.h"

namespace Envoy {
namespace Quic {

class Http3ResponseCodeDetailValues {
public:
  // Invalid HTTP header field was received and stream is going to be
  // closed.
  static constexpr absl::string_view invalid_http_header = "http3.invalid.header.field";
  // The size of headers (or trailers) exceeded the configured limits.
  static constexpr absl::string_view headers_too_large = "http3.headers_too_large";
  // Envoy was configured to drop requests with header keys beginning with underscores.
  static constexpr absl::string_view invalid_underscore = "http3.unexpected_underscore";
  // The peer refused the stream.
  static constexpr absl::string_view remote_refused = "http3.remote_refuse";
  // The peer reset the stream.
  static constexpr absl::string_view remote_reset = "http3.remote_reset";
};

EnvoyQuicServerStream::EnvoyQuicServerStream(
    quic::QuicStreamId id, quic::QuicSpdySession* session, quic::StreamType type,
    envoy::config::core::v3::HttpProtocolOptions::HeadersWithUnderscoresAction
        headers_with_underscores_action)
    : quic::QuicSpdyServerStreamBase(id, session, type),
      EnvoyQuicStream(
          // This should be larger than 8k to fully utilize congestion control
          // window. And no larger than the max stream flow control window for
          // the stream to buffer all the data.
          // Ideally this limit should also correlate to peer's receive window
          // but not fully depends on that.
          16 * 1024, [this]() { runLowWatermarkCallbacks(); },
          [this]() { runHighWatermarkCallbacks(); }),
      headers_with_underscores_action_(headers_with_underscores_action) {}

EnvoyQuicServerStream::EnvoyQuicServerStream(
    quic::PendingStream* pending, quic::QuicSpdySession* session, quic::StreamType type,
    envoy::config::core::v3::HttpProtocolOptions::HeadersWithUnderscoresAction
        headers_with_underscores_action)
    : quic::QuicSpdyServerStreamBase(pending, session, type),
      EnvoyQuicStream(
          // This should be larger than 8k to fully utilize congestion control
          // window. And no larger than the max stream flow control window for
          // the stream to buffer all the data.
          16 * 1024, [this]() { runLowWatermarkCallbacks(); },
          [this]() { runHighWatermarkCallbacks(); }),
      headers_with_underscores_action_(headers_with_underscores_action) {}

void EnvoyQuicServerStream::encode100ContinueHeaders(const Http::ResponseHeaderMap& headers) {
  ASSERT(headers.Status()->value() == "100");
  encodeHeaders(headers, false);
}

void EnvoyQuicServerStream::encodeHeaders(const Http::ResponseHeaderMap& headers, bool end_stream) {
  ENVOY_STREAM_LOG(debug, "encodeHeaders (end_stream={}) {}.", *this, end_stream, headers);
  // QUICHE guarantees to take all the headers. This could cause infinite data to
  // be buffered on headers stream in Google QUIC implementation because
  // headers stream doesn't have upper bound for its send buffer. But in IETF
  // QUIC implementation this is safe as headers are sent on data stream which
  // is bounded by max concurrent streams limited.
  // Same vulnerability exists in crypto stream which can infinitely buffer data
  // if handshake implementation goes wrong.
  // TODO(#8826) Modify QUICHE to have an upper bound for header stream send buffer.
  // This is counting not serialized bytes in the send buffer.
  quic::QuicStream* writing_stream =
      quic::VersionUsesHttp3(transport_version())
          ? static_cast<quic::QuicStream*>(this)
          : (dynamic_cast<quic::QuicSpdySession*>(session())->headers_stream());
  const uint64_t bytes_to_send_old = writing_stream->BufferedDataBytes();

  WriteHeaders(envoyHeadersToSpdyHeaderBlock(headers), end_stream, nullptr);
  local_end_stream_ = end_stream;
  const uint64_t bytes_to_send_new = writing_stream->BufferedDataBytes();
  ASSERT(bytes_to_send_old <= bytes_to_send_new);
  maybeCheckWatermark(bytes_to_send_old, bytes_to_send_new, *filterManagerConnection());
}

void EnvoyQuicServerStream::encodeData(Buffer::Instance& data, bool end_stream) {
  ENVOY_STREAM_LOG(debug, "encodeData (end_stream={}) of {} bytes.", *this, end_stream,
                   data.length());
  if (data.length() == 0 && !end_stream) {
    return;
  }
  ASSERT(!local_end_stream_);
  local_end_stream_ = end_stream;
  // This is counting not serialized bytes in the send buffer.
  const uint64_t bytes_to_send_old = BufferedDataBytes();
  // QUIC stream must take all.
  WriteBodySlices(quic::QuicMemSliceSpan(quic::QuicMemSliceSpanImpl(data)), end_stream);
  if (data.length() > 0) {
    // Send buffer didn't take all the data, threshold needs to be adjusted.
    Reset(quic::QUIC_BAD_APPLICATION_PAYLOAD);
    return;
  }

  const uint64_t bytes_to_send_new = BufferedDataBytes();
  ASSERT(bytes_to_send_old <= bytes_to_send_new);
  maybeCheckWatermark(bytes_to_send_old, bytes_to_send_new, *filterManagerConnection());
}

void EnvoyQuicServerStream::encodeTrailers(const Http::ResponseTrailerMap& trailers) {
  ASSERT(!local_end_stream_);
  local_end_stream_ = true;
  ENVOY_STREAM_LOG(debug, "encodeTrailers: {}.", *this, trailers);
  quic::QuicStream* writing_stream =
      quic::VersionUsesHttp3(transport_version())
          ? static_cast<quic::QuicStream*>(this)
          : (dynamic_cast<quic::QuicSpdySession*>(session())->headers_stream());
  const uint64_t bytes_to_send_old = writing_stream->BufferedDataBytes();
  WriteTrailers(envoyHeadersToSpdyHeaderBlock(trailers), nullptr);
  const uint64_t bytes_to_send_new = writing_stream->BufferedDataBytes();
  ASSERT(bytes_to_send_old <= bytes_to_send_new);
  maybeCheckWatermark(bytes_to_send_old, bytes_to_send_new, *filterManagerConnection());
}

void EnvoyQuicServerStream::encodeMetadata(const Http::MetadataMapVector& /*metadata_map_vector*/) {
  // Metadata Frame is not supported in QUIC.
  // TODO(danzh): add stats for metadata not supported error.
}

void EnvoyQuicServerStream::resetStream(Http::StreamResetReason reason) {
  if (local_end_stream_ && !reading_stopped()) {
    // This is after 200 early response. Reset with QUIC_STREAM_NO_ERROR instead
    // of propagating original reset reason. In QUICHE if a stream stops reading
    // before FIN or RESET received, it resets the steam with QUIC_STREAM_NO_ERROR.

    // Upper layers expect calling resetStream() to immediately raise reset callbacks.
    runResetCallbacks(reason);
    StopReading();
    runResetCallbacks(Http::StreamResetReason::LocalReset);
  } else {
    Reset(envoyResetReasonToQuicRstError(reason));
  }
}

void EnvoyQuicServerStream::switchStreamBlockState(bool should_block) {
  ASSERT(FinishedReadingHeaders(),
         "Upperstream buffer limit is reached before request body is delivered.");
  if (should_block) {
    sequencer()->SetBlockedUntilFlush();
  } else {
    ASSERT(read_disable_counter_ == 0, "readDisable called in between.");
    sequencer()->SetUnblocked();
  }
}

void EnvoyQuicServerStream::OnInitialHeadersComplete(bool fin, size_t frame_len,
                                                     const quic::QuicHeaderList& header_list) {
  // TODO(danzh) Fix in QUICHE. If the stream has been reset in the call stack,
  // OnInitialHeadersComplete() shouldn't be called.
  if (read_side_closed()) {
    return;
  }
  quic::QuicSpdyServerStreamBase::OnInitialHeadersComplete(fin, frame_len, header_list);
  ASSERT(headers_decompressed() && !header_list.empty());
  ENVOY_STREAM_LOG(debug, "Received headers: {}.", *this, header_list.DebugString());
  bool close_connection_upon_invalid_header{false};
  std::unique_ptr<Http::RequestHeaderMapImpl> headers = quicHeadersToEnvoyHeaders<
      Http::RequestHeaderMapImpl>(header_list, [this, &close_connection_upon_invalid_header](
                                                   const std::string& header_name,
                                                   absl::string_view header_value) {
    if (header_name[0] == ':') {
      bool is_valid_header{false};
      for (auto legal_header : {":method", ":scheme", ":authority", ":path", ":protocol"}) {
        if (header_name == legal_header) {
          is_valid_header = true;
          break;
        }
      }
      if (!is_valid_header) {
        ENVOY_STREAM_LOG(debug, "Invalid request header {}", *this, header_name);
        close_connection_upon_invalid_header = true;
        setDetails(Http3ResponseCodeDetailValues::invalid_http_header);
        return HeaderValidationResult::INVALID;
      }
    }
    if (header_name == ":authority" && !Http::HeaderUtility::authorityIsValid(header_value)) {
      close_connection_upon_invalid_header = true;
      setDetails(Http3ResponseCodeDetailValues::invalid_http_header);
      return HeaderValidationResult::INVALID;
    }
    HeaderValidationResult result = checkHeaderNameForUnderscores(header_name);
    if (result != HeaderValidationResult::ACCEPT) {
      close_connection_upon_invalid_header = false;
      setDetails(Http3ResponseCodeDetailValues::invalid_underscore);
      return result;
    }

    if (header_name == "content-length") {
      std::vector<absl::string_view> values = absl::StrSplit(header_value, ',');
      absl::optional<uint64_t> content_length;
      for (const absl::string_view& value : values) {
        uint64_t new_value;
        if (!absl::SimpleAtoi(value, &new_value) || !quiche::QuicheTextUtils::IsAllDigits(value)) {
          ENVOY_STREAM_LOG(debug, "Content length was either unparseable or negative", *this);
          // TODO(danzh) set value according to override_stream_error_on_invalid_http_message from
          // configured http2 options.
          close_connection_upon_invalid_header = true;
          setDetails(Http3ResponseCodeDetailValues::invalid_http_header);
          return HeaderValidationResult::INVALID;
        }
        if (!content_length.has_value()) {
          content_length = new_value;
          continue;
        }
        if (new_value != content_length.value()) {
          ENVOY_STREAM_LOG(
              debug,
              "Parsed content length {} is inconsistent with previously detected content length {}",
              *this, new_value, content_length.value());
          close_connection_upon_invalid_header = false;
          setDetails(Http3ResponseCodeDetailValues::invalid_http_header);
          return HeaderValidationResult::INVALID;
        }
      }
    }
    return HeaderValidationResult::ACCEPT;
  });
  if (headers == nullptr) {
    if (close_connection_upon_invalid_header) {
      stream_delegate()->OnStreamError(quic::QUIC_HTTP_FRAME_ERROR, "Invalid headers");
    } else {
      Reset(quic::QUIC_BAD_APPLICATION_PAYLOAD);
    }
    return;
  }
  if (fin) {
    end_stream_decoded_ = true;
  }
  request_decoder_->decodeHeaders(std::move(headers),
                                  /*end_stream=*/fin);
  ConsumeHeaderList();
}

void EnvoyQuicServerStream::OnBodyAvailable() {
  ASSERT(FinishedReadingHeaders());
  ASSERT(read_disable_counter_ == 0);
  ASSERT(!in_decode_data_callstack_);
  if (read_side_closed()) {
    return;
  }
  in_decode_data_callstack_ = true;

  Buffer::InstancePtr buffer = std::make_unique<Buffer::OwnedImpl>();
  // TODO(danzh): check Envoy per stream buffer limit.
  // Currently read out all the data.
  while (HasBytesToRead()) {
    iovec iov;
    int num_regions = GetReadableRegions(&iov, 1);
    ASSERT(num_regions > 0);
    size_t bytes_read = iov.iov_len;
    buffer->add(iov.iov_base, bytes_read);
    MarkConsumed(bytes_read);
  }

  bool fin_read_and_no_trailers = IsDoneReading();
  // If this call is triggered by an empty frame with FIN which is not from peer
  // but synthesized by stream itself upon receiving HEADERS with FIN or
  // TRAILERS, do not deliver end of stream here. Because either decodeHeaders
  // already delivered it or decodeTrailers will be called.
  bool skip_decoding = (buffer->length() == 0 && !fin_read_and_no_trailers) || end_stream_decoded_;
  if (!skip_decoding) {
    if (fin_read_and_no_trailers) {
      end_stream_decoded_ = true;
    }
    request_decoder_->decodeData(*buffer, fin_read_and_no_trailers);
  }

  if (!sequencer()->IsClosed() || read_side_closed()) {
    in_decode_data_callstack_ = false;
    if (read_disable_counter_ > 0) {
      // If readDisable() was ever called during decodeData() and it meant to disable
      // reading from downstream, the call must have been deferred. Call it now.
      switchStreamBlockState(true);
    }
    return;
  }

  // Trailers may arrived earlier and wait to be consumed after reading all the body. Consume it
  // here.
  maybeDecodeTrailers();

  OnFinRead();
  in_decode_data_callstack_ = false;
}

void EnvoyQuicServerStream::OnTrailingHeadersComplete(bool fin, size_t frame_len,
                                                      const quic::QuicHeaderList& header_list) {
  if (read_side_closed()) {
    return;
  }
  ENVOY_STREAM_LOG(debug, "Received trailers: {}.", *this, received_trailers().DebugString());
  quic::QuicSpdyServerStreamBase::OnTrailingHeadersComplete(fin, frame_len, header_list);
  ASSERT(trailers_decompressed());
  if (session()->connection()->connected() && !rst_sent()) {
    maybeDecodeTrailers();
  }
}

void EnvoyQuicServerStream::OnHeadersTooLarge() {
  ENVOY_STREAM_LOG(debug, "Headers too large.", *this);
  setDetails(Http3ResponseCodeDetailValues::headers_too_large);
  quic::QuicSpdyServerStreamBase::OnHeadersTooLarge();
}

void EnvoyQuicServerStream::maybeDecodeTrailers() {
  if (sequencer()->IsClosed() && !FinishedReadingTrailers()) {
    ASSERT(!received_trailers().empty());
    // Only decode trailers after finishing decoding body.
    ENVOY_STREAM_LOG(debug, "decodeTrailers: {}.", *this, received_trailers().DebugString());
    end_stream_decoded_ = true;
    request_decoder_->decodeTrailers(
        spdyHeaderBlockToEnvoyHeaders<Http::RequestTrailerMapImpl>(received_trailers()));
    MarkTrailersConsumed();
  }
}

void EnvoyQuicServerStream::OnStreamReset(const quic::QuicRstStreamFrame& frame) {
  quic::QuicSpdyServerStreamBase::OnStreamReset(frame);
  Http::StreamResetReason reason = quicRstErrorToEnvoyRemoteResetReason(frame.error_code);
  if (reason == Http::StreamResetReason::RemoteReset ||
      reason == Http::StreamResetReason::ConnectError) {
    setDetails(Http3ResponseCodeDetailValues::remote_reset);
  } else if (reason == Http::StreamResetReason::RemoteRefusedStreamReset) {
    setDetails(Http3ResponseCodeDetailValues::remote_refused);
  }
  runResetCallbacks(reason);
}

void EnvoyQuicServerStream::Reset(quic::QuicRstStreamErrorCode error) {
  // Upper layers expect calling resetStream() to immediately raise reset callbacks.
  runResetCallbacks(quicRstErrorToEnvoyLocalResetReason(error));
  std::cerr << "============= Reset stream with error " << error << "\n";
  quic::QuicSpdyServerStreamBase::Reset(error);
}

void EnvoyQuicServerStream::OnConnectionClosed(quic::QuicErrorCode error,
                                               quic::ConnectionCloseSource source) {
  quic::QuicSpdyServerStreamBase::OnConnectionClosed(error, source);
  runResetCallbacks(Http::StreamResetReason::ConnectionTermination);
}

void EnvoyQuicServerStream::OnClose() {
  quic::QuicSpdyServerStreamBase::OnClose();
  if (BufferedDataBytes() > 0) {
    // If the stream is closed without sending out all buffered data, regard
    // them as sent now and adjust connection buffer book keeping.
    filterManagerConnection()->adjustBytesToSend(0 - BufferedDataBytes());
  }
}

void EnvoyQuicServerStream::OnCanWrite() {
  const uint64_t buffered_data_old = BufferedDataBytes();
  quic::QuicSpdyServerStreamBase::OnCanWrite();
  const uint64_t buffered_data_new = BufferedDataBytes();
  // As long as OnCanWriteNewData() is no-op, data to sent in buffer shouldn't
  // increase.
  ASSERT(buffered_data_new <= buffered_data_old);
  maybeCheckWatermark(buffered_data_old, buffered_data_new, *filterManagerConnection());
}

uint32_t EnvoyQuicServerStream::streamId() { return id(); }

Network::Connection* EnvoyQuicServerStream::connection() { return filterManagerConnection(); }

QuicFilterManagerConnectionImpl* EnvoyQuicServerStream::filterManagerConnection() {
  return dynamic_cast<QuicFilterManagerConnectionImpl*>(session());
}

HeaderValidationResult
EnvoyQuicServerStream::checkHeaderNameForUnderscores(const std::string& header_name) {
  if (headers_with_underscores_action_ == envoy::config::core::v3::HttpProtocolOptions::ALLOW ||
      !Http::HeaderUtility::headerNameContainsUnderscore(header_name)) {
    return HeaderValidationResult::ACCEPT;
  }
  if (headers_with_underscores_action_ ==
      envoy::config::core::v3::HttpProtocolOptions::DROP_HEADER) {
    ENVOY_STREAM_LOG(debug, "Dropping header with invalid characters in its name: {}", *this,
                     header_name);
    // TODO(danzh) Increment dropped_headers_with_underscores_ once http3 stats is propogated;
    return HeaderValidationResult::DROP;
  }
  ENVOY_STREAM_LOG(debug, "Rejecting request due to header name with underscores: {}", *this,
                   header_name);
  // TODO(danzh) Increment requests_rejected_with_underscores_in_headers_ once http3 stats is
  // propogated;
  return HeaderValidationResult::INVALID;
}

} // namespace Quic
} // namespace Envoy
