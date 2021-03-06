#include "sim800l.h"
#include "esphome/core/log.h"
#include <cstring>

namespace esphome {
namespace sim800l {

static const char *const TAG = "sim800l";

const char ASCII_CR = 0x0D;
const char ASCII_LF = 0x0A;

void Sim800LComponent::set_rssi_(sensor::Sensor *rssi) { rssi_ = rssi; }

void Sim800LComponent::update() {
  if (this->watch_dog_++ == 2) {
    this->state_ = STATE_INIT;
    this->write(26);
  }

  if (state_ == STATE_INIT) {
    if (this->registered_ && this->send_pending_) {
      this->send_cmd_("AT+CSCS=\"GSM\"");
      this->state_ = STATE_SENDINGSMS1;
    } else if (this->registered_ && this->dial_pending_) {
      this->send_cmd_("AT+CSCS=\"GSM\"");
      this->state_ = STATE_DIALING1;
    } else {
      this->send_cmd_("AT");
      this->state_ = STATE_CHECK_AT;
    }
    this->expect_ack_ = true;
  }

  if (state_ == STATE_RECEIVEDSMS) {
    // Serial Buffer should have flushed.
    // Send cmd to delete received sms
    char delete_cmd[20];
    sprintf(delete_cmd, "AT+CMGD=%d", this->parse_index_);
    this->send_cmd_(delete_cmd);
    this->state_ = STATE_CHECK_SMS;
    this->expect_ack_ = true;
  }
}

void Sim800LComponent::send_cmd_(const std::string &message) {
  ESP_LOGVV(TAG, "S: %s - %d", message.c_str(), this->state_);
  this->watch_dog_ = 0;
  this->write_str(message.c_str());
  this->write_byte(ASCII_LF);
}

void Sim800LComponent::parse_cmd_(std::string message) {
  ESP_LOGVV(TAG, "R: %s - %d", message.c_str(), this->state_);

  if (message.empty())
    return;

  if (this->expect_ack_) {
    bool ok = message == "OK";
    this->expect_ack_ = false;
    if (!ok) {
      if (this->state_ == STATE_CHECK_AT && message == "AT") {
        // Expected ack but AT echo received
        this->state_ = STATE_DISABLE_ECHO;
        this->expect_ack_ = true;
      } else {
        ESP_LOGW(TAG, "Not ack. %d %s", this->state_, message.c_str());
        this->state_ = STATE_IDLE;  // Let it timeout
        return;
      }
    }
  }

  switch (this->state_) {
    case STATE_INIT: {
      // While we were waiting for update to check for messages, this notifies a message
      // is available.
      bool message_available = message.compare(0, 6, "+CMTI:") == 0;
      if (!message_available)
        break;
      // Else fall thru ...
    }
    case STATE_CHECK_SMS:
      send_cmd_("AT+CMGL=\"ALL\"");
      this->state_ = STATE_PARSE_SMS_RESPONSE;
      this->parse_index_ = 0;
      this->expect_ack_ = false;
      break;
    case STATE_DISABLE_ECHO:
      send_cmd_("ATE0");
      this->state_ = STATE_CHECK_AT;
      this->expect_ack_ = true;
      break;
    case STATE_CHECK_AT:
      send_cmd_("AT+CMGF=1");
      this->state_ = STATE_CREG;
      this->expect_ack_ = true;
      break;
    case STATE_CREG:
      send_cmd_("AT+CREG?");
      this->state_ = STATE_CREGWAIT;
      break;
    case STATE_CREGWAIT: {
      // Response: "+CREG: 0,1" -- the one there means registered ok
      //           "+CREG: -,-" means not registered ok
      this->registered_ = message.compare(0, 6, "+CREG:") == 0 && (message[9] == '1' || message[9] == '5');
      if (this->registered_) {
        this->callback_reg.call();
        ESP_LOGVV(TAG, "Registered OK");
        this->state_ = STATE_CSQ;
        this->expect_ack_ = true;
      } else {
        ESP_LOGVV(TAG, "Registration Fail");
        if (message[7] == '0') {  // Network registration is disabled, enable it
          send_cmd_("AT+CREG=1");
          this->expect_ack_ = true;
          this->state_ = STATE_CHECK_AT;
        } else {
          // Keep waiting registration
          this->state_ = STATE_CREG;
        }
      }
      break;
    }
    case STATE_CSQ:
      send_cmd_("AT+CSQ");
      this->state_ = STATE_CSQ_RESPONSE;
      break;
    case STATE_CSQ_RESPONSE:
      if (message.compare(0, 5, "+CSQ:") == 0) {
        size_t comma = message.find(',', 6);
        if (comma != 6) {
          int rssi = strtol(message.substr(6, comma - 6).c_str(), nullptr, 10);
          if (this->rssi_ != nullptr) {
            this->rssi_->publish_state(rssi);
            ESP_LOGVV(TAG, "RSSI: %d", rssi);
          }
        }
      }
      this->expect_ack_ = true;
      this->state_ = STATE_CHECK_SMS;
      break;
    case STATE_PARSE_SMS_RESPONSE:
      if (message.compare(0, 6, "+CMGL:") == 0 && this->parse_index_ == 0) {
        ESP_LOGVV(TAG, "Found CMGL");
        size_t start = 7;
        size_t end = message.find(',', start);
        uint8_t item = 0;

        while (end != start) {
          ESP_LOGVV(TAG, "SMS parsing item %d", item);
          item++;
          if (item == 1) {  // Slot Index
            this->parse_index_ = strtol(message.substr(start, end - start).c_str(), nullptr, 10);
          }
          // item 2 = STATUS, usually "REC UNERAD"
          if (item == 3) {  // recipient
            // Add 1 and remove 2 from substring to get rid of "quotes"
            this->sender_ = message.substr(start + 1, end - start - 2);
            break;
          }
          // item 4 = ""
          // item 5 = Received timestamp
          start = end + 1;
          end = message.find(',', start);
        }

        if (item < 2) {
          ESP_LOGI(TAG, "Invalid message %d %s", this->state_, message.c_str());
          return;
        }
        this->state_ = STATE_RECEIVESMS;
      }
      // Otherwise we receive another OK, we do nothing just wait polling to continuously check for SMS
      if (message == "OK")
        this->state_ = STATE_INIT;
      break;
    case STATE_RECEIVESMS:
      /* Our recipient is set and the message body is in message
        kick ESPHome callback now
      */
      if (message == "OK" || message.compare(0, 6, "+CMGL:") == 0) {
        ESP_LOGI(TAG, "Received SMS from: %s", this->sender_.c_str());
        ESP_LOGI(TAG, "Mesage: %s", this->message_.c_str());
        this->callback_.call(this->message_, this->sender_);
        this->state_ = STATE_RECEIVEDSMS;
        this->sender_.erase();
        this->message_.erase();
      } else {
        this->message_.append(message.c_str());
        this->message_.append("\n");
      }
      break;
    case STATE_SENDINGSMS1:
      this->send_cmd_("AT+CMGS=\"" + this->recipient_ + "\"");
      this->state_ = STATE_SENDINGSMS2;
      break;
    case STATE_SENDINGSMS2:
      if (message == ">") {
        // Send sms body
        ESP_LOGD(TAG, "Sending message: '%s'", this->outgoing_message_.c_str());
        this->write_str(this->outgoing_message_.c_str());
        this->write(26);
        this->state_ = STATE_SENDINGSMS3;
      } else {
        this->registered_ = false;
        this->state_ = STATE_INIT;
        this->send_cmd_("AT+CMEE=2");
        this->write(26);
      }
      break;
    case STATE_SENDINGSMS3:
      if (message.compare(0, 6, "+CMGS:") == 0) {
        ESP_LOGD(TAG, "SMS Sent OK: %s", message.c_str());
        this->send_pending_ = false;
        this->state_ = STATE_CHECK_SMS;
        this->expect_ack_ = true;
      }
      break;
    case STATE_DIALING1:
      this->send_cmd_("ATD" + this->recipient_ + ';');
      this->state_ = STATE_DIALING2;
      break;
    case STATE_DIALING2:
      if (message == "OK") {
        // Dialing
        ESP_LOGD(TAG, "Dialing: '%s'", this->recipient_.c_str());
        this->state_ = STATE_INIT;
        this->dial_pending_ = false;
      } else {
        this->registered_ = false;
        this->state_ = STATE_INIT;
        this->send_cmd_("AT+CMEE=2");
        this->write(26);
      }
      break;
    default:
      ESP_LOGW(TAG, "Unhandled: %s - %d", message.c_str(), this->state_);
      break;
  }
}

void Sim800LComponent::loop() {
  // Read message
  while (this->available()) {
    uint8_t byte;
    this->read_byte(&byte);

    if (this->read_pos_ == SIM800L_READ_BUFFER_LENGTH)
      this->read_pos_ = 0;

    ESP_LOGVV(TAG, "Buffer pos: %u %d", this->read_pos_, byte);  // NOLINT

    if (byte == ASCII_CR)
      continue;
    if (byte >= 0x7F)
      byte = '?';  // need to be valid utf8 string for log functions.
    this->read_buffer_[this->read_pos_] = byte;

    if (this->state_ == STATE_SENDINGSMS2 && this->read_pos_ == 0 && byte == '>')
      this->read_buffer_[++this->read_pos_] = ASCII_LF;

    if (this->read_buffer_[this->read_pos_] == ASCII_LF) {
      this->read_buffer_[this->read_pos_] = 0;
      this->read_pos_ = 0;
      this->parse_cmd_(this->read_buffer_);
    } else {
      this->read_pos_++;
    }
  }
}

void Sim800LComponent::send_sms(const std::string &recipient, const std::string &message) {
  ESP_LOGD(TAG, "Sending to %s: %s", recipient.c_str(), message.c_str());
  this->recipient_ = recipient;
  this->outgoing_message_ = message;
  this->send_pending_ = true;
  this->update();
}

void Sim800LComponent::delete_sms() {
  this->send_cmd_("at+cmgda=\"del all\"\n\r");
  this->state_ = STATE_CHECK_SMS;
  this->expect_ack_ = true;
  this->update();
}

void Sim800LComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "SIM800L:");
  ESP_LOGCONFIG(TAG, "  RSSI: %f dB", this->rssi_->get_state());
}
void Sim800LComponent::dial(const std::string &recipient) {
  ESP_LOGD(TAG, "Dialing %s", recipient.c_str());
  this->recipient_ = recipient;
  this->dial_pending_ = true;
  this->update();
}

}  // namespace sim800l
}  // namespace esphome