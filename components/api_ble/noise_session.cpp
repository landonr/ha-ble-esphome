#include "noise_session.h"

#ifdef USE_API_BLE
#ifdef USE_API_BLE_NOISE
#ifdef USE_ESP32

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <cstring>

namespace esphome::api_ble {

static const char *const TAG = "api_ble.noise";

static const char *const PROLOGUE_INIT = "NoiseAPIInit";
static constexpr size_t PROLOGUE_INIT_LEN = 12;  // strlen("NoiseAPIInit")

/// Largest frame we assemble locally: server hello (1 proto byte + name NUL +
/// 13-byte mac buffer) or a handshake message (1 success byte + 64) or a
/// reject (1 failure byte + reason); all fit comfortably.
static constexpr size_t MAX_TX_HANDSHAKE_FRAME = 128;

NoiseSession::NoiseSession(const std::array<uint8_t, 32> &psk) : psk_(psk) {
  this->prologue_.assign(PROLOGUE_INIT, PROLOGUE_INIT + PROLOGUE_INIT_LEN);
}

NoiseSession::~NoiseSession() {
  if (this->handshake_ != nullptr) {
    noise_handshakestate_free(this->handshake_);
  }
  if (this->send_cipher_ != nullptr) {
    noise_cipherstate_free(this->send_cipher_);
  }
  if (this->recv_cipher_ != nullptr) {
    noise_cipherstate_free(this->recv_cipher_);
  }
}

bool NoiseSession::check_noise_(int err, const char *what) {
  if (err == 0)
    return true;
  char errbuf[64];
  noise_strerror(err, errbuf, sizeof(errbuf));
  ESP_LOGW(TAG, "%s failed: %s (%d)", what, errbuf, err);
  this->state_ = State::FAILED;
  return false;
}

bool NoiseSession::on_handshake_frame(BLEBytePipe &pipe, const uint8_t *body, uint16_t len) {
  switch (this->state_) {
    case State::CLIENT_HELLO: {
      // Contents ignored (future flags); the framed hello (u16 BE size +
      // body) extends the prologue, which both sides must compute
      // identically.
      size_t old_size = this->prologue_.size();
      this->prologue_.resize(old_size + 2 + len);
      this->prologue_[old_size] = static_cast<uint8_t>(len >> 8);
      this->prologue_[old_size + 1] = static_cast<uint8_t>(len);
      if (len > 0) {
        std::memcpy(this->prologue_.data() + old_size + 2, body, len);
      }
      if (!this->send_server_hello_(pipe))
        return false;
      if (!this->init_handshake_())
        return false;
      this->state_ = State::HANDSHAKE;
      return this->run_handshake_actions_(pipe);
    }
    case State::HANDSHAKE: {
      // Handshake messages carry a leading error byte: 0x00 = success.
      if (len == 0) {
        this->send_reject(pipe, "Empty handshake message");
        this->state_ = State::FAILED;
        return false;
      }
      if (body[0] != 0x00) {
        ESP_LOGW(TAG, "Bad handshake error byte: %u", body[0]);
        this->send_reject(pipe, "Bad handshake error byte");
        this->state_ = State::FAILED;
        return false;
      }
      NoiseBuffer mbuf;
      noise_buffer_init(mbuf);
      noise_buffer_set_input(mbuf, const_cast<uint8_t *>(body) + 1, len - 1);
      int err = noise_handshakestate_read_message(this->handshake_, &mbuf, nullptr);
      if (err != 0) {
        this->send_reject(pipe, err == NOISE_ERROR_MAC_FAILURE ? "Handshake MAC failure" : "Handshake error");
        return this->check_noise_(err, "noise_handshakestate_read_message");
      }
      return this->run_handshake_actions_(pipe);
    }
    default:
      ESP_LOGW(TAG, "Handshake frame in bad state %u", static_cast<unsigned>(this->state_));
      this->state_ = State::FAILED;
      return false;
  }
}

bool NoiseSession::send_server_hello_(BLEBytePipe &pipe) {
  // Body: chosen proto (0x01) + node name NUL + node mac NUL.
  const auto &name = App.get_name();
  char mac[MAC_ADDRESS_BUFFER_SIZE];
  get_mac_address_into_buffer(mac);

  size_t name_len = name.size() + 1;  // including NUL
  uint8_t msg[MAX_TX_HANDSHAKE_FRAME];
  size_t total_size = 1 + name_len + MAC_ADDRESS_BUFFER_SIZE;
  if (total_size > sizeof(msg)) {
    // Device name too long for a handshake frame; cannot happen with
    // ESPHome's 31-char name limit, but fail closed.
    this->state_ = State::FAILED;
    return false;
  }
  msg[0] = 0x01;
  std::memcpy(msg + 1, name.c_str(), name_len);
  std::memcpy(msg + 1 + name_len, mac, MAC_ADDRESS_BUFFER_SIZE);
  return this->write_handshake_frame_(pipe, msg, total_size);
}

bool NoiseSession::init_handshake_() {
  NoiseProtocolId nid;
  std::memset(&nid, 0, sizeof(nid));
  // Noise_NNpsk0_25519_ChaChaPoly_SHA256
  nid.pattern_id = NOISE_PATTERN_NN;
  nid.cipher_id = NOISE_CIPHER_CHACHAPOLY;
  nid.dh_id = NOISE_DH_CURVE25519;
  nid.prefix_id = NOISE_PREFIX_STANDARD;
  nid.hybrid_id = NOISE_DH_NONE;
  nid.hash_id = NOISE_HASH_SHA256;
  nid.modifier_ids[0] = NOISE_MODIFIER_PSK0;

  int err = noise_handshakestate_new_by_id(&this->handshake_, &nid, NOISE_ROLE_RESPONDER);
  if (!this->check_noise_(err, "noise_handshakestate_new_by_id"))
    return false;
  err = noise_handshakestate_set_pre_shared_key(this->handshake_, this->psk_.data(), this->psk_.size());
  if (!this->check_noise_(err, "noise_handshakestate_set_pre_shared_key"))
    return false;
  err = noise_handshakestate_set_prologue(this->handshake_, this->prologue_.data(), this->prologue_.size());
  if (!this->check_noise_(err, "noise_handshakestate_set_prologue"))
    return false;
  // set_prologue copies into the handshake state.
  this->prologue_.clear();
  this->prologue_.shrink_to_fit();
  err = noise_handshakestate_start(this->handshake_);
  return this->check_noise_(err, "noise_handshakestate_start");
}

bool NoiseSession::run_handshake_actions_(BLEBytePipe &pipe) {
  while (true) {
    int action = noise_handshakestate_get_action(this->handshake_);
    if (action == NOISE_ACTION_READ_MESSAGE)
      return true;  // wait for the client's next frame
    if (action == NOISE_ACTION_WRITE_MESSAGE) {
      uint8_t buffer[65];
      NoiseBuffer mbuf;
      noise_buffer_init(mbuf);
      noise_buffer_set_output(mbuf, buffer + 1, sizeof(buffer) - 1);
      int err = noise_handshakestate_write_message(this->handshake_, &mbuf, nullptr);
      if (!this->check_noise_(err, "noise_handshakestate_write_message"))
        return false;
      buffer[0] = 0x00;  // success byte
      if (!this->write_handshake_frame_(pipe, buffer, mbuf.size + 1))
        return false;
      continue;
    }
    if (action == NOISE_ACTION_SPLIT) {
      int err = noise_handshakestate_split(this->handshake_, &this->send_cipher_, &this->recv_cipher_);
      if (!this->check_noise_(err, "noise_handshakestate_split"))
        return false;
      if (noise_cipherstate_get_mac_length(this->send_cipher_) != MAC_SIZE) {
        ESP_LOGE(TAG, "Unexpected MAC length");
        this->state_ = State::FAILED;
        return false;
      }
      noise_handshakestate_free(this->handshake_);
      this->handshake_ = nullptr;
      this->state_ = State::DATA;
      ESP_LOGD(TAG, "Handshake complete");
      return true;
    }
    ESP_LOGW(TAG, "Bad handshake action: %d", action);
    this->state_ = State::FAILED;
    return false;
  }
}

void NoiseSession::send_reject(BLEBytePipe &pipe, const char *reason) {
  // 0x01 failure byte + reason text (in-tree explicit reject format).
  uint8_t data[32];
  data[0] = 0x01;
  size_t reason_len = std::min(std::strlen(reason), sizeof(data) - 1);
  std::memcpy(data + 1, reason, reason_len);
  this->write_handshake_frame_(pipe, data, reason_len + 1);
}

bool NoiseSession::write_handshake_frame_(BLEBytePipe &pipe, const uint8_t *body, uint16_t len) {
  // Assemble header + body contiguously so the TX ring write is atomic.
  uint8_t frame[FRAME_HEADER_SIZE + MAX_TX_HANDSHAKE_FRAME];
  frame[0] = 0x01;
  frame[1] = static_cast<uint8_t>(len >> 8);
  frame[2] = static_cast<uint8_t>(len);
  std::memcpy(frame + FRAME_HEADER_SIZE, body, len);
  if (!pipe.write(frame, FRAME_HEADER_SIZE + len)) {
    this->state_ = State::FAILED;
    return false;
  }
  return true;
}

bool NoiseSession::decrypt(uint8_t *body, uint16_t len, uint16_t &type_out, const uint8_t *&payload_out,
                           uint16_t &len_out) {
  if (this->state_ != State::DATA)
    return false;
  NoiseBuffer mbuf;
  noise_buffer_init(mbuf);
  noise_buffer_set_inout(mbuf, body, len, len);
  int err = noise_cipherstate_decrypt(this->recv_cipher_, &mbuf);
  if (!this->check_noise_(err, "noise_cipherstate_decrypt"))
    return false;
  if (mbuf.size < 4) {
    ESP_LOGW(TAG, "Bad data packet: size %u too short", static_cast<unsigned>(mbuf.size));
    this->state_ = State::FAILED;
    return false;
  }
  const uint16_t type = (static_cast<uint16_t>(body[0]) << 8) | body[1];
  const uint16_t data_len = (static_cast<uint16_t>(body[2]) << 8) | body[3];
  if (data_len > mbuf.size - 4) {
    ESP_LOGW(TAG, "Bad data packet: data_len %u > %u", data_len, static_cast<unsigned>(mbuf.size - 4));
    this->state_ = State::FAILED;
    return false;
  }
  type_out = type;
  payload_out = body + 4;
  len_out = data_len;
  return true;
}

uint16_t NoiseSession::encrypt(uint8_t *buf, uint16_t payload_size, uint8_t msg_type) {
  if (this->state_ != State::DATA)
    return 0;
  buf[0] = 0x01;
  // buf[1..2]: encrypted size, filled in below.
  buf[3] = 0;  // type high byte (all current message types fit in 8 bits)
  buf[4] = msg_type;
  buf[5] = static_cast<uint8_t>(payload_size >> 8);
  buf[6] = static_cast<uint8_t>(payload_size);

  NoiseBuffer mbuf;
  noise_buffer_init(mbuf);
  noise_buffer_set_inout(mbuf, buf + FRAME_HEADER_SIZE, 4 + payload_size, 4 + payload_size + MAC_SIZE);
  int err = noise_cipherstate_encrypt(this->send_cipher_, &mbuf);
  if (!this->check_noise_(err, "noise_cipherstate_encrypt"))
    return 0;
  buf[1] = static_cast<uint8_t>(mbuf.size >> 8);
  buf[2] = static_cast<uint8_t>(mbuf.size);
  return FRAME_HEADER_SIZE + mbuf.size;
}

extern "C" {
/// noise-c random source: the RF-subsystem HWRNG via esphome::random_bytes.
void noise_rand_bytes(void *output, size_t len) {
  if (!esphome::random_bytes(reinterpret_cast<uint8_t *>(output), len)) {
    ESP_LOGE(TAG, "Acquiring random bytes failed; rebooting");
    arch_restart();
  }
}
}

}  // namespace esphome::api_ble

#endif  // USE_ESP32
#endif  // USE_API_BLE_NOISE
#endif  // USE_API_BLE
