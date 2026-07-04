#pragma once

#include "esphome/core/defines.h"

#ifdef USE_API_BLE
#ifdef USE_API_BLE_NOISE
#ifdef USE_ESP32

#include "ble_byte_pipe.h"

#include <noise/protocol.h>

#include <array>
#include <cstdint>
#include <vector>

namespace esphome::api_ble {

/// Server-side Noise_NNpsk0_25519_ChaChaPoly_SHA256 session over a
/// BLEBytePipe. Lean transcription of the in-tree APINoiseFrameHelper
/// (api_frame_helper_noise.cpp) minus the socket plumbing: the connection's
/// frame parser hands in complete frame bodies, responses are written as
/// complete frames into the TX ring. Wire format is byte-identical, so
/// aioesphomeapi / the stock esphome integration work unchanged.
///
/// Handshake sequence (server = responder):
///   client hello frame (body absorbed into the prologue)
///   -> server hello frame (0x01 proto byte + name NUL + mac NUL)
///   -> NNpsk0 exchange (each message prefixed with a 0x00 success byte;
///      rejects are 0x01 + reason text)
///   -> split into send/recv cipher states, data phase.
class NoiseSession {
 public:
  /// Wire overhead per data frame: 1 indicator + 2 size (unencrypted frame
  /// header) then 2 type + 2 data_len (encrypted message header) before the
  /// payload; ChaChaPoly MAC after it.
  static constexpr uint8_t FRAME_HEADER_SIZE = 3;
  static constexpr uint8_t HEADER_PADDING = FRAME_HEADER_SIZE + 4;
  static constexpr uint8_t MAC_SIZE = 16;
  /// Handshake-phase frames are limited to this size (in-tree limit).
  static constexpr uint16_t MAX_HANDSHAKE_SIZE = 128;

  explicit NoiseSession(const std::array<uint8_t, 32> &psk);
  ~NoiseSession();

  NoiseSession(const NoiseSession &) = delete;
  NoiseSession &operator=(const NoiseSession &) = delete;

  bool in_data_phase() const { return this->state_ == State::DATA; }

  /// Handle one complete handshake-phase frame body. Responses (server
  /// hello, handshake messages, explicit rejects) are written into `pipe`.
  /// Returns false on any failure -- the caller must tear the session down.
  bool on_handshake_frame(BLEBytePipe &pipe, const uint8_t *body, uint16_t len);

  /// Send an explicit handshake reject frame (0x01 failure byte + reason).
  void send_reject(BLEBytePipe &pipe, const char *reason);

  /// Decrypt a data-phase frame body in place. Outputs the message type and
  /// a payload view pointing into `body`. Returns false on failure.
  bool decrypt(uint8_t *body, uint16_t len, uint16_t &type_out, const uint8_t *&payload_out, uint16_t &len_out);

  /// Encrypt an outgoing message in place. `buf` points at the start of the
  /// HEADER_PADDING region; the payload already sits at buf + HEADER_PADDING
  /// with MAC_SIZE bytes of slack after it. Returns the total frame length
  /// from `buf`, or 0 on failure.
  uint16_t encrypt(uint8_t *buf, uint16_t payload_size, uint8_t msg_type);

 protected:
  enum class State : uint8_t { CLIENT_HELLO, HANDSHAKE, DATA, FAILED };

  bool init_handshake_();
  bool send_server_hello_(BLEBytePipe &pipe);
  /// Run noise actions until the handshake needs more input (READ), splits
  /// into cipher states (-> DATA), or fails.
  bool run_handshake_actions_(BLEBytePipe &pipe);
  bool write_handshake_frame_(BLEBytePipe &pipe, const uint8_t *body, uint16_t len);
  /// Log a noise-c error and fail the session; true when err == 0.
  bool check_noise_(int err, const char *what);

  NoiseHandshakeState *handshake_{nullptr};
  NoiseCipherState *send_cipher_{nullptr};
  NoiseCipherState *recv_cipher_{nullptr};
  /// "NoiseAPIInit" + framed client hello; released into the handshake state.
  std::vector<uint8_t> prologue_;
  const std::array<uint8_t, 32> &psk_;
  State state_{State::CLIENT_HELLO};
};

}  // namespace esphome::api_ble

#endif  // USE_ESP32
#endif  // USE_API_BLE_NOISE
#endif  // USE_API_BLE
