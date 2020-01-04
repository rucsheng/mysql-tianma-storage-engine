/* Copyright (c) 2018, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA */

#include "sql/binlog_reader.h"
#include "sql/event_crypt.h"
#include "sql/log_event.h"

unsigned char *Default_binlog_event_allocator::allocate(size_t size) {
  DBUG_EXECUTE_IF("simulate_allocate_failure", return nullptr;);
  return static_cast<unsigned char *>(
      my_malloc(key_memory_log_event, size + 1, MYF(MY_WME)));
}

void Default_binlog_event_allocator::deallocate(unsigned char *ptr) {
  my_free(ptr);
}

#ifndef DBUG_OFF
static void debug_corrupt_event(unsigned char *buffer, unsigned int event_len) {
  /*
    Corrupt the event.
    Dump threads need to exclude Format_description_log_event,
    Previous_gtids_log_event and Gtid_log_event
    events from injected corruption to allow dump thread to move forward
    on binary log until the missing transactions from slave when
    MASTER_AUTO_POSITION= 1.
  */
  DBUG_EXECUTE_IF(
      "corrupt_read_log_event", unsigned char type = buffer[EVENT_TYPE_OFFSET];
      if (type != binary_log::FORMAT_DESCRIPTION_EVENT &&
          type != binary_log::PREVIOUS_GTIDS_LOG_EVENT &&
          type != binary_log::GTID_LOG_EVENT &&
          type != binary_log::START_ENCRYPTION_EVENT) {
        int cor_pos = rand() % (event_len - BINLOG_CHECKSUM_LEN -
                                LOG_EVENT_MINIMAL_HEADER_LEN) +
                      LOG_EVENT_MINIMAL_HEADER_LEN;
        buffer[cor_pos] = buffer[cor_pos] + 1;
        DBUG_PRINT("info", ("Corrupt the event on position %d", cor_pos));
      });
}
#endif  // ifdef DBUG_OFF

bool Binlog_event_data_istream::start_decryption(
    binary_log::Start_encryption_event *see) {
  DBUG_ASSERT(!crypto_data.is_enabled());

  Start_encryption_log_event *sele =
      down_cast<Start_encryption_log_event *>(see);
  if (!sele->is_valid() ||
      crypto_data.init(see->crypto_scheme, see->key_version, see->nonce)) {
    m_error->set_type(Binlog_read_error::DECRYPT_INIT_FAILURE);
    return true;
  }
  return false;
}

Binlog_event_data_istream::Binlog_event_data_istream(
    Binlog_read_error *error, Basic_istream *istream,
    unsigned int max_event_size)
    : m_error(error), m_istream(istream), m_max_event_size(max_event_size) {}

bool Binlog_event_data_istream::read_event_header() {
  return read_fixed_length<Binlog_read_error::READ_EOF>(
      m_header, LOG_EVENT_MINIMAL_HEADER_LEN);
}

Binlog_event_data_istream::Decryption_buffer::~Decryption_buffer() {
  resize(0);
}

bool Binlog_event_data_istream::Decryption_buffer::resize(size_t new_size) {
  memset_s(m_buffer, m_size, 0, m_size);
  delete[] m_buffer;
  m_size = 0;
  m_buffer = nullptr;
  if (new_size == 0) {
    return false;
  }
  m_buffer = new (std::nothrow) uchar[new_size];
  if (m_buffer == nullptr) {
    return true;
  }
  m_size = new_size;
  return false;
}

bool Binlog_event_data_istream::Decryption_buffer::set_size(
    size_t size_to_set) {
  if (size_to_set == m_size) {
    return false;
  }
  if (size_to_set > m_size) {
    return resize(size_to_set);
  }
  DBUG_ASSERT(size_to_set < m_size);

  if (size_to_set < (m_size / 2) &&
      ++m_number_of_events_with_half_the_size == 100) {
    // There were already 101 events which size was a half of currently
    // allocated size. This is strong indication that we had occured an
    // event which was unusually big. Shrink the buffer to half the size.
    if (resize(m_size / 2)) {
      return true;
    }
    m_number_of_events_with_half_the_size = 0;
  }
  return false;
}

uchar *Binlog_event_data_istream::Decryption_buffer::data() { return m_buffer; }

bool Binlog_event_data_istream::fill_event_data(
    unsigned char *event_data, bool verify_checksum,
    enum_binlog_checksum_alg checksum_alg) {
  memcpy(event_data, m_header, LOG_EVENT_MINIMAL_HEADER_LEN);
  if (read_fixed_length<Binlog_read_error::TRUNC_EVENT>(
          event_data + LOG_EVENT_MINIMAL_HEADER_LEN,
          m_event_length - LOG_EVENT_MINIMAL_HEADER_LEN))
    return true;

  if (crypto_data.is_enabled()) {
    // crypto only works on binlog files
    Basic_binlog_ifile *binlog_file =
        down_cast<Basic_binlog_ifile *>(m_istream);

    // if file position if larger than 4 bytes we still care only about
    // least significant 4 bytes
    if (m_decryption_buffer.set_size(m_event_length) ||
        decrypt_event(
            static_cast<uint32_t>((binlog_file->position() - m_event_length)),
            crypto_data, event_data, m_decryption_buffer.data(),
            m_event_length)) {
      return m_error->set_type(Binlog_read_error::DECRYPT);
    }

    memcpy(event_data, m_decryption_buffer.data(), m_event_length);
  }

#ifndef DBUG_OFF
  debug_corrupt_event(event_data, m_event_length);
#endif

  if (verify_checksum) {
    if (event_data[EVENT_TYPE_OFFSET] == binary_log::FORMAT_DESCRIPTION_EVENT)
      checksum_alg = Log_event_footer::get_checksum_alg(
          reinterpret_cast<char *>(event_data), m_event_length);

    if (Log_event_footer::event_checksum_test(event_data, m_event_length,
                                              checksum_alg) &&
        !DBUG_EVALUATE_IF("simulate_unknown_ignorable_log_event", 1, 0)) {
      return m_error->set_type(crypto_data.is_enabled()
                                   ? Binlog_read_error::DECRYPT
                                   : Binlog_read_error::CHECKSUM_FAILURE);
    }
  }
  return false;
}

bool Binlog_event_data_istream::check_event_header() {
  m_event_length = uint4korr(m_header + EVENT_LEN_OFFSET);

  if (m_event_length < LOG_EVENT_MINIMAL_HEADER_LEN)
    return m_error->set_type(Binlog_read_error::BOGUS);
  if (m_event_length > m_max_event_size)
    return m_error->set_type(Binlog_read_error::EVENT_TOO_LARGE);

  return false;
}

Binlog_read_error::Error_type binlog_event_deserialize(
    const unsigned char *buffer, unsigned int event_len,
    const Format_description_event *fde, bool verify_checksum,
    Log_event **event, bool force_opt MY_ATTRIBUTE((unused))) {
  const char *buf = reinterpret_cast<const char *>(buffer);
  Log_event *ev = NULL;
  enum_binlog_checksum_alg alg;

  DBUG_ENTER("binlog_event_deserialize");

  DBUG_ASSERT(fde != 0);
  DBUG_PRINT("info", ("binlog_version: %d", fde->binlog_version));
  DBUG_DUMP("data", (unsigned char *)buf, event_len);

  /* Check the integrity */
  if (event_len < LOG_EVENT_MINIMAL_HEADER_LEN) {
    DBUG_PRINT("error", ("event_len=%u", event_len));
    DBUG_RETURN(Binlog_read_error::TRUNC_EVENT);
  }

  if (event_len != uint4korr(buf + EVENT_LEN_OFFSET)) {
    DBUG_PRINT("error",
               ("event_len=%u EVENT_LEN_OFFSET=%d "
                "buf[EVENT_TYPE_OFFSET]=%d ENUM_END_EVENT=%d "
                "uint4korr(buf+EVENT_LEN_OFFSET)=%d",
                event_len, EVENT_LEN_OFFSET, buf[EVENT_TYPE_OFFSET],
                binary_log::ENUM_END_EVENT, uint4korr(buf + EVENT_LEN_OFFSET)));
    DBUG_RETURN(event_len > uint4korr(buf + EVENT_LEN_OFFSET)
                    ? Binlog_read_error::BOGUS
                    : Binlog_read_error::TRUNC_EVENT);
  }

  uchar event_type = buf[EVENT_TYPE_OFFSET];

  /*
    Sanity check for Format description event. This is needed because
    get_checksum_alg will assume that Format_description_event is well-formed
  */
  if (event_type == binary_log::FORMAT_DESCRIPTION_EVENT) {
    if (event_len <= LOG_EVENT_MINIMAL_HEADER_LEN + ST_COMMON_HEADER_LEN_OFFSET)
      DBUG_RETURN(Binlog_read_error::TRUNC_FD_EVENT);

    uint tmp_header_len =
        buf[LOG_EVENT_MINIMAL_HEADER_LEN + ST_COMMON_HEADER_LEN_OFFSET];
    if (event_len < tmp_header_len + ST_SERVER_VER_OFFSET + ST_SERVER_VER_LEN)
      DBUG_RETURN(Binlog_read_error::TRUNC_FD_EVENT);
  }

  /*
    If it is a FD event, then uses the checksum algorithm in it. Otherwise use
    the checksum algorithm in fde provided by caller.

    Notice, a pre-checksum FD version forces alg := BINLOG_CHECKSUM_ALG_UNDEF.
  */
  alg = (event_type != binary_log::FORMAT_DESCRIPTION_EVENT)
            ? fde->footer()->checksum_alg
            : Log_event_footer::get_checksum_alg(buf, event_len);

#ifndef DBUG_OFF
  binary_log_debug::debug_checksum_test =
      DBUG_EVALUATE_IF("simulate_checksum_test_failure", true, false);
#endif

  if (verify_checksum &&
      Log_event_footer::event_checksum_test((uchar *)buf, event_len, alg) &&
      /* Skip the crc check when simulating an unknown ignorable log event. */
      !DBUG_EVALUATE_IF("simulate_unknown_ignorable_log_event", 1, 0)) {
    DBUG_RETURN(Binlog_read_error::CHECKSUM_FAILURE);
  }

  if (event_type > fde->number_of_event_types &&
      event_type != binary_log::START_ENCRYPTION_EVENT &&
      /*
        Skip the event type check when simulating an unknown ignorable event.
      */
      !DBUG_EVALUATE_IF("simulate_unknown_ignorable_log_event", 1, 0)) {
    /*
      It is unsafe to use the fde if its post_header_len
      array does not include the event type.
    */
    DBUG_PRINT("error", ("event type %d found, but the current "
                         "Format_description_event supports only %d event "
                         "types, plus Start Encryption Event",
                         event_type, fde->number_of_event_types));
    DBUG_RETURN(Binlog_read_error::INVALID_EVENT);
  }

  /* Remove checksum length from event_len */
  if (alg != binary_log::BINLOG_CHECKSUM_ALG_UNDEF &&
      (event_type == binary_log::FORMAT_DESCRIPTION_EVENT ||
       alg != binary_log::BINLOG_CHECKSUM_ALG_OFF))
    event_len = event_len - BINLOG_CHECKSUM_LEN;

  switch (event_type) {
    case binary_log::QUERY_EVENT:
#ifndef DBUG_OFF
      binary_log_debug::debug_query_mts_corrupt_db_names =
          DBUG_EVALUATE_IF("query_log_event_mts_corrupt_db_names", true, false);
#endif
      ev = new Query_log_event(buf, fde, binary_log::QUERY_EVENT);
      break;
    case binary_log::ROTATE_EVENT:
      ev = new Rotate_log_event(buf, fde);
      break;
    case binary_log::APPEND_BLOCK_EVENT:
      ev = new Append_block_log_event(buf, fde);
      break;
    case binary_log::DELETE_FILE_EVENT:
      ev = new Delete_file_log_event(buf, fde);
      break;
    case binary_log::STOP_EVENT:
      ev = new Stop_log_event(buf, fde);
      break;
    case binary_log::INTVAR_EVENT:
      ev = new Intvar_log_event(buf, fde);
      break;
    case binary_log::XID_EVENT:
      ev = new Xid_log_event(buf, fde);
      break;
    case binary_log::RAND_EVENT:
      ev = new Rand_log_event(buf, fde);
      break;
    case binary_log::USER_VAR_EVENT:
      ev = new User_var_log_event(buf, fde);
      break;
    case binary_log::FORMAT_DESCRIPTION_EVENT:
      ev = new Format_description_log_event(buf, fde);
      break;
    case binary_log::WRITE_ROWS_EVENT_V1:
      if (!(fde->post_header_len.empty()))
        ev = new Write_rows_log_event(buf, fde);
      break;
    case binary_log::UPDATE_ROWS_EVENT_V1:
      if (!(fde->post_header_len.empty()))
        ev = new Update_rows_log_event(buf, fde);
      break;
    case binary_log::DELETE_ROWS_EVENT_V1:
      if (!(fde->post_header_len.empty()))
        ev = new Delete_rows_log_event(buf, fde);
      break;
    case binary_log::TABLE_MAP_EVENT:
      if (!(fde->post_header_len.empty()))
        ev = new Table_map_log_event(buf, fde);
      break;
    case binary_log::BEGIN_LOAD_QUERY_EVENT:
      ev = new Begin_load_query_log_event(buf, fde);
      break;
    case binary_log::EXECUTE_LOAD_QUERY_EVENT:
      ev = new Execute_load_query_log_event(buf, fde);
      break;
    case binary_log::INCIDENT_EVENT:
      ev = new Incident_log_event(buf, fde);
      break;
    case binary_log::ROWS_QUERY_LOG_EVENT:
      ev = new Rows_query_log_event(buf, fde);
      break;
    case binary_log::GTID_LOG_EVENT:
    case binary_log::ANONYMOUS_GTID_LOG_EVENT:
      ev = new Gtid_log_event(buf, fde);
      break;
    case binary_log::PREVIOUS_GTIDS_LOG_EVENT:
      ev = new Previous_gtids_log_event(buf, fde);
      break;
    case binary_log::WRITE_ROWS_EVENT:
      ev = new Write_rows_log_event(buf, fde);
      break;
    case binary_log::UPDATE_ROWS_EVENT:
      ev = new Update_rows_log_event(buf, fde);
      break;
    case binary_log::DELETE_ROWS_EVENT:
      ev = new Delete_rows_log_event(buf, fde);
      break;
    case binary_log::TRANSACTION_CONTEXT_EVENT:
      ev = new Transaction_context_log_event(buf, fde);
      break;
    case binary_log::VIEW_CHANGE_EVENT:
      ev = new View_change_log_event(buf, fde);
      break;
    case binary_log::XA_PREPARE_LOG_EVENT:
      ev = new XA_prepare_log_event(buf, fde);
      break;
    case binary_log::PARTIAL_UPDATE_ROWS_EVENT:
      ev = new Update_rows_log_event(buf, fde);
      break;
    case binary_log::START_ENCRYPTION_EVENT:
      ev = new Start_encryption_log_event(buf, fde);
      break;
    default:
      /*
        Create an object of Ignorable_log_event for unrecognized sub-class.
        So that SLAVE SQL THREAD will only update the position and continue.
      */
      if (uint2korr(buf + FLAGS_OFFSET) & LOG_EVENT_IGNORABLE_F) {
        ev = new Ignorable_log_event(buf, fde);
      } else {
        DBUG_PRINT("error",
                   ("Unknown event code: %d", (int)buf[EVENT_TYPE_OFFSET]));
        ev = NULL;
      }
      break;
  }

  /*
    is_valid is used for small event-specific sanity tests which are
    important; for example there are some my_malloc() in constructors
    (e.g. Query_log_event::Query_log_event(char*...)); when these
    my_malloc() fail we can't return an error out of the constructor
    (because constructor is "void") ; so instead we leave the pointer we
    wanted to allocate (e.g. 'query') to 0 and we test it and set the
    value of is_valid to true or false based on the test.
    Same for Format_description_log_event, member 'post_header_len'.
  */
  if (!ev || !ev->is_valid()) {
    delete ev;
    DBUG_RETURN(Binlog_read_error::INVALID_EVENT);
  }

  ev->common_footer->checksum_alg = alg;
  if (ev->common_footer->checksum_alg != binary_log::BINLOG_CHECKSUM_ALG_OFF &&
      ev->common_footer->checksum_alg != binary_log::BINLOG_CHECKSUM_ALG_UNDEF)
    ev->crc = uint4korr(buf + event_len);

  DBUG_PRINT("read_event", ("%s(type_code: %d; event_len: %d)",
                            ev ? ev->get_type_str() : "<unknown>",
                            buf[EVENT_TYPE_OFFSET], event_len));
  *event = ev;
  DBUG_RETURN(Binlog_read_error::SUCCESS);
}