#pragma once

#include "internal.hpp"

#include <sqlite3.h>

namespace aurora::sqlite {

#if defined(__SWITCH__)
// Horizon (SD card through newlib, WAL compiled out): a rollback journal kept next to the database
// and no syncs, as SwitchWaker runs its caches (switch/native/aurora/patches/0006 at 92307ff). WAL
// fell back to DELETE, where every commit failed with "disk I/O error" on the console. PERSIST
// creates the -journal file once and zeroes its header at each commit; with the build's exclusive
// locking it stays open. A commit cut short by HOME is rolled back from it at the next open.
inline constexpr const char* CachePragmas =
    "PRAGMA journal_mode=PERSIST; PRAGMA journal_size_limit=4194304; PRAGMA synchronous=OFF;";
#else
// WAL mode + NORMAL = no need for disk syncs, consistent but not durable is fine.
inline constexpr const char* CachePragmas = "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;";
#endif

inline int exec(sqlite3* db, const char* sql) { return sqlite3_exec(db, sql, nullptr, nullptr, nullptr); }

template <typename T>
int exec(sqlite3* db, const char* sql, T callback, char** errmsg = nullptr) {
  return sqlite3_exec(
      db, sql,
      [](void* cb, int argc, char** argv, char** columns) -> int {
        auto& fp = *static_cast<T*>(cb);
        if constexpr (std::is_same_v<decltype(fp(0, 0, 0)), void>) {
          fp(argc, argv, columns);
          return 0;
        } else {
          return fp(argc, argv, columns);
        }
      },
      &callback, errmsg);
}

class Transaction {
public:
  Transaction(sqlite3* db, Module& log, bool immediate = false) : m_db(db), m_log(log) {
    const auto type = immediate ? "BEGIN IMMEDIATE" : "BEGIN";
    const auto ret = sqlite3_exec(m_db, type, nullptr, nullptr, nullptr);
    if (ret != SQLITE_OK) {
      m_log.error("Failed to start transaction: {}", sqlite3_errmsg(m_db));
      return;
    }
    m_active = true;
  }

  Transaction(const Transaction&) = delete;
  Transaction& operator=(const Transaction&) = delete;
  Transaction(Transaction&&) = delete;
  Transaction& operator=(Transaction&&) = delete;

  ~Transaction() {
    if (!m_active) {
      return;
    }

    const auto ret = sqlite3_exec(m_db, "ROLLBACK", nullptr, nullptr, nullptr);
    if (ret != SQLITE_OK) {
      m_log.error("Failed to roll back transaction (uh oh?): {}", sqlite3_errmsg(m_db));
    }
  }

  void commit() {
    if (!m_active) {
      return;
    }

    const auto ret = sqlite3_exec(m_db, "COMMIT", nullptr, nullptr, nullptr);
    if (ret != SQLITE_OK) {
      m_log.error("Failed to commit transaction: {} ({})", sqlite3_errmsg(m_db), sqlite3_extended_errcode(m_db));
      return;
    }

    m_active = false;
  }

  explicit operator bool() const { return m_active; }

private:
  sqlite3* m_db = nullptr;
  Module& m_log;
  bool m_active = false;
};

} // namespace aurora::sqlite
