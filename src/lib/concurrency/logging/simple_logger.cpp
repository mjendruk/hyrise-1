#include "simple_logger.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sstream>

#include "all_type_variant.hpp"
#include "logger.hpp"
#include "text_recovery.hpp"

namespace opossum {

void SimpleLogger::commit(const TransactionID transaction_id, std::function<void(TransactionID)> callback) {
  std::stringstream ss;
  ss << "(t," << transaction_id << ")\n";
  _write_to_logfile(ss);
}

void SimpleLogger::value(const TransactionID transaction_id, const std::string table_name, const RowID row_id,
                          const std::vector<AllTypeVariant> values) {
  // std::stringstream ss;
  // ss << "(v," << transaction_id << "," << table_name << "," << row_id << "," << values.str() << ")\n";
  // _write_to_logfile(ss);
}

void SimpleLogger::invalidate(const TransactionID transaction_id, const std::string table_name, const RowID row_id) {
  std::stringstream ss;
  ss << "(i," << transaction_id << "," << table_name << "," << row_id << ")\n";
  _write_to_logfile(ss);
}

void SimpleLogger::flush() { fsync(_file_descriptor); }

void SimpleLogger::_write_to_logfile(const std::stringstream& ss) {
  _file_mutex.lock();
  write(_file_descriptor, reinterpret_cast<const void*>(ss.str().c_str()), ss.str().length());
  _file_mutex.unlock();
}

void SimpleLogger::recover() { TextRecovery::getInstance().recover(); }

SimpleLogger::SimpleLogger() : AbstractLogger() {
  std::string path = Logger::directory + Logger::filename;

  // TODO: what if directory does not exists?

  int oflags = O_WRONLY | O_APPEND | O_CREAT;

  // read and write rights needed, since default rights do not allow to reopen the file after restarting the db
  mode_t mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;

  _file_descriptor = open(path.c_str(), oflags, mode);

  DebugAssert(_file_descriptor != -1, "Logfile could not be opened or created: " + path);
}

}  // namespace opossum