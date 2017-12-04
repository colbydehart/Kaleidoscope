/* -*- mode: c++ -*-
 * Kaleidoscope-Qukeys -- Assign two keycodes to a single key
 * Copyright (C) 2017  Michael Richters
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <Kaleidoscope.h>
#include <Kaleidoscope-Qukeys.h>
#include <kaleidoscope/hid.h>
#include <MultiReport/Keyboard.h>

#ifdef ARDUINO_VIRTUAL
#define debug_print(...) printf(__VA_ARGS__)
#else
#define debug_print(...)
#endif


namespace kaleidoscope {

Qukey::Qukey(int8_t layer, byte row, byte col, Key alt_keycode) {
  this->layer = layer;
  this->addr = addr::addr(row, col);
  this->alt_keycode = alt_keycode;
}

Qukey * Qukeys::qukeys_;
uint8_t Qukeys::qukeys_count_ = 0;
bool Qukeys::active_ = true;
uint16_t Qukeys::time_limit_ = 500;
QueueItem Qukeys::key_queue_[QUKEYS_QUEUE_MAX] = {};
uint8_t Qukeys::key_queue_length_ = 0;
byte Qukeys::qukey_state_[] = {};

// Empty constructor; nothing is stored at the instance level
Qukeys::Qukeys(void) {}

int8_t Qukeys::lookupQukey(uint8_t key_addr) {
  if (key_addr == QUKEY_UNKNOWN_ADDR) {
    return QUKEY_NOT_FOUND;
  }
  for (int8_t i = 0; i < qukeys_count_; i++) {
    if (qukeys_[i].addr == key_addr) {
      byte row = addr::row(key_addr);
      byte col = addr::col(key_addr);
      if ((qukeys_[i].layer == QUKEY_ALL_LAYERS) ||
          (qukeys_[i].layer == Layer.lookupActiveLayer(row, col))) {
        return i;
      }
    }
  }
  return QUKEY_NOT_FOUND;
}

void Qukeys::enqueue(uint8_t key_addr) {
  if (key_queue_length_ == QUKEYS_QUEUE_MAX) {
    flushKey(QUKEY_STATE_PRIMARY, IS_PRESSED | WAS_PRESSED);
    flushQueue();
  }
  key_queue_[key_queue_length_].addr = key_addr;
  key_queue_[key_queue_length_].flush_time = millis() + time_limit_;
  key_queue_length_++;
  addr::mask(key_addr);
}

int8_t Qukeys::searchQueue(uint8_t key_addr) {
  for (int8_t i = 0; i < key_queue_length_; i++) {
    if (key_queue_[i].addr == key_addr)
      return i;
  }
  return QUKEY_NOT_FOUND;
}

// flush a single entry from the head of the queue
void Qukeys::flushKey(bool qukey_state, uint8_t keyswitch_state) {
  addr::unmask(key_queue_[0].addr);
  int8_t qukey_index = lookupQukey(key_queue_[0].addr);
  if (qukey_index != QUKEY_NOT_FOUND) {
    setQukeyState(key_queue_[0].addr, qukey_state);
  }
  byte row = addr::row(key_queue_[0].addr);
  byte col = addr::col(key_queue_[0].addr);
  Key keycode = Key_NoKey;
  if (qukey_state == QUKEY_STATE_ALTERNATE && qukey_index != QUKEY_NOT_FOUND) {
    keycode = qukeys_[qukey_index].alt_keycode;
  } else {
    keycode = Layer.lookup(row, col);
  }

  // Since we're in the middle of the key scan, we don't necessarily
  // have a full HID report, and we don't want to accidentally turn
  // off keys that the scan hasn't reached yet, so we force the
  // current report to be the same as the previous one, then proceed
  HID_KeyboardReport_Data_t hid_report;
  // First, save the current report
  memcpy(hid_report.allkeys, Keyboard.keyReport.allkeys, sizeof(hid_report));
  // Next, copy the old report
  memcpy(Keyboard.keyReport.allkeys, Keyboard.lastKeyReport.allkeys, sizeof(Keyboard.keyReport));
  // Instead of just calling pressKey here, we start processing the
  // key again, as if it was just pressed, and mark it as injected, so
  // we can ignore it and don't start an infinite loop. It would be
  // nice if we could use key_state to also indicate which plugin
  // injected the key.
  handleKeyswitchEvent(keycode, row, col, IS_PRESSED | INJECTED);
  // Now we send the report (if there were any changes)
  hid::sendKeyboardReport();

  /* I think this is now unnecessary
  // Now for the tricky bit; we need to know if the key was actually
  // released, or if it's still being held. Otherwise, we'll screw up
  // the next call to flushKey().
  if (keyToggledOff(keyswitch_state)) {
    handleKeyswitchEvent(keycode, row, col, keyswitch_state | INJECTED);
    hid::sendKeyboardReport();
  }
  */

  // Next, we restore the current state of the report
  memcpy(Keyboard.keyReport.allkeys, hid_report.allkeys, sizeof(hid_report));

  // Last, if the key is still down, add its code back in
  if (! keyToggledOn(keyswitch_state))
    handleKeyswitchEvent(keycode, row, col, IS_PRESSED | WAS_PRESSED | INJECTED);

  // Shift the queue, so key_queue[0] is always the first key that gets processed
  for (byte i = 0; i < key_queue_length_; i++) {
    key_queue_[i] = key_queue_[i + 1];
  }
  key_queue_length_--;
}

// flushQueue() is called when a key that's in the key_queue is
// released. This means that all the keys ahead of it in the queue are
// still being held, so first we flush them, then we flush the
// released key (with different parameters).
void Qukeys::flushQueue(int8_t index) {
  if (index == QUKEY_NOT_FOUND)
    return;
  for (int8_t i = 0; i < index; i++) {
    if (key_queue_length_ == 0)
      break;
    flushKey(QUKEY_STATE_ALTERNATE, IS_PRESSED | WAS_PRESSED);
  }
  flushKey(QUKEY_STATE_PRIMARY, WAS_PRESSED);
}

// Flush all the non-qukey keys from the front of the queue
void Qukeys::flushQueue(void) {
  // flush keys until we find a qukey:
  while (key_queue_length_ > 0 &&
         lookupQukey(key_queue_[0].addr) == QUKEY_NOT_FOUND) {
    flushKey(QUKEY_STATE_PRIMARY, IS_PRESSED | WAS_PRESSED);
  }
}

Key Qukeys::keyScanHook(Key mapped_key, byte row, byte col, uint8_t key_state) {
  // Uncomment this for debugging, so as not to make flashing difficult
  //if (row == 0 && col == 0) return mapped_key;

  // If Qukeys is turned off, continue to next plugin
  if (!active_)
    return mapped_key;

  // If the key was injected (from the queue being flushed), continue to next plugin
  if (key_state & INJECTED)
    return mapped_key;

  // If the key isn't active, and didn't just toggle off, continue to next plugin
  if (!keyIsPressed(key_state) && !keyWasPressed(key_state))
    return mapped_key;

  // get key addr & qukey (if any)
  uint8_t key_addr = addr::addr(row, col);
  int8_t qukey_index = lookupQukey(key_addr);

  // If the key was just pressed:
  if (keyToggledOn(key_state)) {
    // If the queue is empty and the key isn't a qukey, proceed:
    if (key_queue_length_ == 0 &&
        qukey_index == QUKEY_NOT_FOUND)
      return mapped_key;
    // Otherwise, queue the key and stop processing:
    enqueue(key_addr);
    return Key_NoKey;
  }

  // In all other cases, we need to know if the key is queued already
  int8_t queue_index = searchQueue(key_addr);

  // If the key was just released:
  if (keyToggledOff(key_state)) {
    // If the key isn't in the key_queue, proceed
    if (queue_index == QUKEY_NOT_FOUND) {
      return mapped_key;
    }
    flushQueue(queue_index);
    return mapped_key;
  }

  // Otherwise, the key is still pressed

  // If the key is not a qukey:
  if (qukey_index == QUKEY_NOT_FOUND) {
    // If the key was pressed before the keys in the queue, proceed:
    if (queue_index == QUKEY_NOT_FOUND) {
      return mapped_key;
    } else {
      // suppress this keypress; it's still in the queue
      return Key_NoKey;
    }
  }

  // If the qukey is not in the queue, check its state
  if (queue_index == QUKEY_NOT_FOUND) {
    if (getQukeyState(key_addr) == QUKEY_STATE_ALTERNATE) {
      return qukeys_[qukey_index].alt_keycode;
    } else { // qukey_state == QUKEY_STATE_PRIMARY
      return mapped_key;
    }
  }
  // else state is undetermined; block. I could check timeouts here,
  // but I'd rather do that in the pre-report hook
  return Key_NoKey;
}

void Qukeys::preReportHook(void) {
  // If the qukey has been held longer than the time limit, set its
  // state to the alternate keycode and add it to the report
  uint32_t current_time = millis();
  while (key_queue_length_ > 0) {
    if (lookupQukey(key_queue_[0].addr) == QUKEY_NOT_FOUND) {
      flushKey(QUKEY_STATE_PRIMARY, IS_PRESSED | WAS_PRESSED);
    } else if (current_time > key_queue_[0].flush_time) {
      flushKey(QUKEY_STATE_ALTERNATE, IS_PRESSED | WAS_PRESSED);
    } else {
      break;
    }
  }
}

void Qukeys::loopHook(bool post_clear) {
  if (!post_clear)
    return preReportHook();
}

void Qukeys::begin() {
  // initializing the key_queue seems unnecessary, actually
  for (int8_t i = 0; i < QUKEYS_QUEUE_MAX; i++) {
    key_queue_[i].addr = QUKEY_UNKNOWN_ADDR;
    key_queue_[i].flush_time = 0;
  }
  key_queue_length_ = 0;

  Kaleidoscope.useEventHandlerHook(keyScanHook);
  Kaleidoscope.useLoopHook(loopHook);
}

} // namespace kaleidoscope {

kaleidoscope::Qukeys Qukeys;
