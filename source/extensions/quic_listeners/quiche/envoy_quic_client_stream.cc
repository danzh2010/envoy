#include "extensions/quic_listeners/quiche/envoy_quic_client_stream.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif

#include "quiche/quic/core/quic_session.h"
#include "quiche/quic/core/http/quic_header_list.h"
#include "quiche/spdy/core/spdy_header_block.h"
#include "extensions/quic_listeners/quiche/platform/quic_mem_slice_span_impl.h"
#include "quiche/common/platform/api/quiche_text_utils.h"

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "extensions/quic_listeners/quiche/envoy_quic_utils.h"
#include "extensions/quic_listeners/quiche/envoy_quic_client_session.h"

#include "common/buffer/buffer_impl.h"
#include "common/http/codes.h"
#include "common/http/header_map_impl.h"
#include "common/http/header_utility.h"
#include "common/http/utility.h"
#include "common/common/assert.h"
#include "common/common/enum_to_int.h"

#include "server/backtrace.h"

namespace Envoy {
namespace Quic {

EnvoyQuicClientStream::EnvoyQuicClientStream(quic::QuicStreamId id,
                                             quic::QuicSpdyClientSession* client_session,
                                             quic::StreamType type)
    : quic::QuicSpdyClientStream(id, client_session, type),
      EnvoyQuicStream(
          // This should be larger than 8k to fully utilize congestion control
          // window. And no larger than the max stream flow control window for
          // the stream to buffer all the data.
          // Ideally this limit should also correlate to peer's receive window
          // but not fully depends on that.
          16 * 1024, [this]() { runLowWatermarkCallbacks(); },
          [this]() { runHighWatermarkCallbacks(); }) {}

EnvoyQuicClientStream::EnvoyQuicClientStream(quic::PendingStream* pending,
                                             quic::QuicSpdyClientSession* client_session,
                                             quic::StreamType type)
    : quic::QuicSpdyClientStream(pending, client_session, type),
      EnvoyQuicStream(
          16 * 1024, [this]() { runLowWatermarkCallbacks(); },
          [this]() { runHighWatermarkCallbacks(); }) {}

Http::Status EnvoyQuicClientStream::encodeHeaders(const Http::RequestHeaderMap& headers,
                                                  bool end_stream) {
  // Required headers must be present. This can only happen by some erroneous processing after the
  // downstream codecs decode.
  RETURN_IF_ERROR(Http::HeaderUtility::checkRequiredHeaders(headers));
  ENVOY_STREAM_LOG(debug, "encodeHeaders: (end_stream={}) {}.", *this, end_stream, headers);
  quic::QuicStream* writing_stream =
      quic::VersionUsesHttp3(transport_version())
          ? static_cast<quic::QuicStream*>(this)
          : (dynamic_cast<quic::QuicSpdySession*>(session())->headers_stream());
  const uint64_t bytes_to_send_old = writing_stream->BufferedDataBytes();
  auto spdy_headers = envoyHeadersToSpdyHeaderBlock(headers);
  if (headers.Method() && headers.Method()->value() == "CONNECT") {
    // It is a bytestream connect and should have :path and :protocol set accordingly
    // As HTTP/1.1 does not require a path for CONNECT, we may have to add one
    // if shifting codecs. For now, default to "/" - this can be made
    // configurable if necessary.
    // https://tools.ietf.org/html/draft-kinnear-httpbis-http2-transport-02
    spdy_headers[":protocol"] = Http::Headers::get().ProtocolValues.Bytestream;
    if (!headers.Path()) {
      spdy_headers[":path"] = "/";
    }
  }
  WriteHeaders(std::move(spdy_headers), end_stream, nullptr);
  local_end_stream_ = end_stream;
  const uint64_t bytes_to_send_new = writing_stream->BufferedDataBytes();
  ASSERT(bytes_to_send_old <= bytes_to_send_new);
  // IETF QUIC sends HEADER frame on current stream. After writing headers, the
  // buffer may increase.
  maybeCheckWatermark(bytes_to_send_old, bytes_to_send_new, *filterManagerConnection());
  return Http::okStatus();
}

void EnvoyQuicClientStream::encodeData(Buffer::Instance& data, bool end_stream) {
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

void EnvoyQuicClientStream::encodeTrailers(const Http::RequestTrailerMap& trailers) {
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
  // IETF QUIC sends HEADER frame on current stream. After writing trailers, the
  // buffer may increase.
  maybeCheckWatermark(bytes_to_send_old, bytes_to_send_new, *filterManagerConnection());
}

void EnvoyQuicClientStream::encodeMetadata(const Http::MetadataMapVector& /*metadata_map_vector*/) {
  // Metadata Frame is not supported in QUIC.
  // TODO(danzh): add stats for metadata not supported error.
}

void EnvoyQuicClientStream::resetStream(Http::StreamResetReason reason) {
  Reset(envoyResetReasonToQuicRstError(reason));
}

void EnvoyQuicClientStream::switchStreamBlockState(bool should_block) {
  ASSERT(FinishedReadingHeaders(),
         "Upper stream buffer limit is reached before response body is delivered.");
  if (should_block) {
    sequencer()->SetBlockedUntilFlush();
  } else {
    ASSERT(read_disable_counter_ == 0, "readDisable called in between.");
    sequencer()->SetUnblocked();
  }
}

void EnvoyQuicClientStream::OnInitialHeadersComplete(bool fin, size_t frame_len,
                                                     const quic::QuicHeaderList& header_list) {
  if (read_side_closed()) {
    return;
  }
  quic::QuicSpdyStream::OnInitialHeadersComplete(fin, frame_len, header_list);
  ASSERT(headers_decompressed() && !header_list.empty());

  ENVOY_STREAM_LOG(debug, "decodeHeaders: {}.", *this, header_list.DebugString());
  if (fin) {
    end_stream_decoded_ = true;
  }
  bool close_connection_upon_invalid_header;
  std::unique_ptr<Http::ResponseHeaderMapImpl> headers = quicHeadersToEnvoyHeaders<
      Http::ResponseHeaderMapImpl>(header_list, [this, &close_connection_upon_invalid_header](
                                                    const std::string& header_name,
                                                    absl::string_view header_value) {
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
          return HeaderValidationResult::INVALID;
        }
      }
    }
    return HeaderValidationResult::ACCEPT;
  });
  if (headers == nullptr) {
    Reset(quic::QUIC_BAD_APPLICATION_PAYLOAD);
    return;
  }

  const uint64_t status = Http::Utility::getResponseStatus(*headers);
  if (status >= 100 && status < 200) {
    // These are Informational 1xx headers, not the actual response headers.
    ENVOY_STREAM_LOG(debug, "Received informational response code: {}", *this, status);
    set_headers_decompressed(false);
    if (status == 100 && !decoded_100_continue_) {
      // This is 100 Continue, only decode it once to support Expect:100-Continue header.
      decoded_100_continue_ = true;
      response_decoder_->decode100ContinueHeaders(std::move(headers));
    }
  } else {
    response_decoder_->decodeHeaders(std::move(headers),
                                     /*end_stream=*/fin);
  }

  ConsumeHeaderList();
}

void EnvoyQuicClientStream::OnBodyAvailable() {
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
  ASSERT(buffer->length() == 0 || !end_stream_decoded_);

  bool fin_read_and_no_trailers = IsDoneReading();
  // If this call is triggered by an empty frame with FIN which is not from peer
  // but synthesized by stream itself upon receiving HEADERS with FIN or
  // TRAILERS, do not deliver end of stream here. Because either decodeHeaders
  // already delivered it or decodeTrailers will be called.
  bool skip_decoding = (buffer->length() == 0 && !fin_read_and_no_trailers) || end_stream_decoded_;
  if (!skip_decoding) {
    std::cerr << " ============ decode data fin_read_and_no_trailers = " << fin_read_and_no_trailers
              << " buffer length " << buffer->length() << "\n";
    if (fin_read_and_no_trailers) {
      end_stream_decoded_ = true;
    }
    response_decoder_->decodeData(*buffer, fin_read_and_no_trailers);
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

void EnvoyQuicClientStream::OnTrailingHeadersComplete(bool fin, size_t frame_len,
                                                      const quic::QuicHeaderList& header_list) {
  if (read_side_closed()) {
    return;
  }
  ENVOY_STREAM_LOG(debug, "Received trailers: {}.", *this, header_list.DebugString());
  quic::QuicSpdyStream::OnTrailingHeadersComplete(fin, frame_len, header_list);
  ASSERT(trailers_decompressed());
  if (session()->connection()->connected() && !rst_sent()) {
    maybeDecodeTrailers();
  }
}

void EnvoyQuicClientStream::maybeDecodeTrailers() {
  if (sequencer()->IsClosed() && !FinishedReadingTrailers()) {
    ASSERT(!received_trailers().empty());
    end_stream_decoded_ = true;
    ENVOY_STREAM_LOG(debug, "decodeTrailers: {}.", *this, received_trailers().DebugString());
    // Only decode trailers after finishing decoding body.
    end_stream_decoded_ = true;
    response_decoder_->decodeTrailers(
        spdyHeaderBlockToEnvoyHeaders<Http::ResponseTrailerMapImpl>(received_trailers()));
    MarkTrailersConsumed();
  }
}

void EnvoyQuicClientStream::OnStreamReset(const quic::QuicRstStreamFrame& frame) {
  quic::QuicSpdyClientStream::OnStreamReset(frame);
  runResetCallbacks(quicRstErrorToEnvoyRemoteResetReason(frame.error_code));
}

void EnvoyQuicClientStream::Reset(quic::QuicRstStreamErrorCode error) {
  std::cerr << "=============== Reset with error " << error << "\n";
  // Upper layers expect calling resetStream() to immediately raise reset callbacks.
  runResetCallbacks(quicRstErrorToEnvoyLocalResetReason(error));
  quic::QuicSpdyClientStream::Reset(error);
}

void EnvoyQuicClientStream::OnConnectionClosed(quic::QuicErrorCode error,
                                               quic::ConnectionCloseSource source) {
  quic::QuicSpdyClientStream::OnConnectionClosed(error, source);
  if (!end_stream_decoded_) {
    runResetCallbacks(Http::StreamResetReason::ConnectionTermination);
    // runResetCallbacks(quicErrorCodeToEnvoyResetReason(error));
  }
}

void EnvoyQuicClientStream::OnClose() {
  quic::QuicSpdyClientStream::OnClose();
  if (BufferedDataBytes() > 0) {
    // If the stream is closed without sending out all buffered data, regard
    // them as sent now and adjust connection buffer book keeping.
    filterManagerConnection()->adjustBytesToSend(0 - BufferedDataBytes());
  }
}

void EnvoyQuicClientStream::OnCanWrite() {
  uint64_t buffered_data_old = BufferedDataBytes();
  quic::QuicSpdyClientStream::OnCanWrite();
  uint64_t buffered_data_new = BufferedDataBytes();
  // As long as OnCanWriteNewData() is no-op, data to sent in buffer shouldn't
  // increase.
  ASSERT(buffered_data_new <= buffered_data_old);
  maybeCheckWatermark(buffered_data_old, buffered_data_new, *filterManagerConnection());
}

uint32_t EnvoyQuicClientStream::streamId() { return id(); }

Network::Connection* EnvoyQuicClientStream::connection() { return filterManagerConnection(); }

QuicFilterManagerConnectionImpl* EnvoyQuicClientStream::filterManagerConnection() {
  return dynamic_cast<QuicFilterManagerConnectionImpl*>(session());
}

} // namespace Quic
} // namespace Envoy
