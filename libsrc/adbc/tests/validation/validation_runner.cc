/*
 *  validation_runner.cc
 *
 *  ADBC upstream validation harness runner for the Virtuoso driver.
 *
 *  Phase 10: links against libvirtadbc.so and the vendored ADBC
 *  validation suite (GTest-based). A VirtuosoQuirks subclass encodes
 *  Virtuoso's dialect, type-mapping, and feature set so the harness
 *  knows what to expect.
 *
 *  Usage:
 *    VIRT_ADBC_TEST_URI=virtuoso://user:pass@host:port \
 *      ./validation_runner [--gtest_filter=...]
 *
 *  This file is part of the OpenLink Software Virtuoso Open-Source (VOS)
 *  project.
 *
 *  Copyright (C) 1998-2026 OpenLink Software
 *
 *  Licensed under the Apache 2.0 license (matching arrow-adbc) so the
 *  upstream can re-absorb this file when the driver lands in
 *  apache/arrow-adbc.
 */

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nanoarrow/nanoarrow.hpp>

#include "adbc_validation.h"
#include "adbc_validation_util.h"

using adbc_validation::Handle;
using adbc_validation::IsOkStatus;
using adbc_validation::IsStatus;

/* ====================================================================
 *  VirtuosoQuirks
 *
 *  Tells the upstream validation harness:
 *    - How to set up a test database / drop tables / create samples
 *    - What SQL dialect (positional "?" bindings, DDL syntax)
 *    - Which ADBC 1.1.0 features are supported
 *    - How Virtuoso types round-trip through Arrow
 * ==================================================================== */

class VirtuosoQuirks : public adbc_validation::DriverQuirks {
 public:
  AdbcStatusCode SetupDatabase(struct AdbcDatabase* database,
                               struct AdbcError* error) const override {
    const char* uri = std::getenv("VIRT_ADBC_TEST_URI");
    if (!uri) {
      ADD_FAILURE() << "Must provide env var VIRT_ADBC_TEST_URI";
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
    return AdbcDatabaseSetOption(database, "uri", uri, error);
  }

  AdbcStatusCode DropTable(struct AdbcConnection* connection,
                           const std::string& name,
                           struct AdbcError* error) const override {
    adbc_validation::Handle<struct AdbcStatement> statement;
    RAISE_ADBC(AdbcStatementNew(connection, &statement.value, error));

    std::string query = "DROP TABLE \"" + name + "\"";
    RAISE_ADBC(AdbcStatementSetSqlQuery(&statement.value, query.c_str(), error));
    RAISE_ADBC(AdbcStatementExecuteQuery(&statement.value, nullptr, nullptr, error));
    return AdbcStatementRelease(&statement.value, error);
  }

  AdbcStatusCode DropTable(struct AdbcConnection* connection,
                           const std::string& name,
                           const std::string& db_schema,
                           struct AdbcError* error) const override {
    adbc_validation::Handle<struct AdbcStatement> statement;
    RAISE_ADBC(AdbcStatementNew(connection, &statement.value, error));

    std::string query =
        "DROP TABLE \"" + db_schema + "\".\"" + name + "\"";
    RAISE_ADBC(AdbcStatementSetSqlQuery(&statement.value, query.c_str(), error));
    RAISE_ADBC(AdbcStatementExecuteQuery(&statement.value, nullptr, nullptr, error));
    return AdbcStatementRelease(&statement.value, error);
  }

  AdbcStatusCode DropTempTable(struct AdbcConnection* connection,
                               const std::string& name,
                               struct AdbcError* error) const override {
    /* Virtuoso temporary tables live in the current connection scope;
     * "DROP TABLE" works on them too.                                   */
    return DropTable(connection, name, error);
  }

  std::string BindParameter(int index) const override {
    (void) index;
    return "?";     /* Virtuoso uses positional ? */
  }

  ArrowType IngestSelectRoundTripType(ArrowType ingest_type) const override {
    switch (ingest_type) {
      /* Virtuoso stores BOOL as SMALLINT and returns INT16. */
      case NANOARROW_TYPE_BOOL:
        return NANOARROW_TYPE_INT16;
      /* Virtuoso promotes narrow integer types. */
      case NANOARROW_TYPE_INT8:
      case NANOARROW_TYPE_UINT8:
        return NANOARROW_TYPE_INT16;
      case NANOARROW_TYPE_UINT16:
      case NANOARROW_TYPE_UINT32:
        return NANOARROW_TYPE_INT32;
      case NANOARROW_TYPE_UINT64:
        return NANOARROW_TYPE_INT64;
      /* Virtuoso maps float16 -> float. */
      case NANOARROW_TYPE_HALF_FLOAT:
        return NANOARROW_TYPE_FLOAT;
      /* Large/view strings/binary collapse to the base type via ODBC. */
      case NANOARROW_TYPE_LARGE_STRING:
      case NANOARROW_TYPE_STRING_VIEW:
        return NANOARROW_TYPE_STRING;
      case NANOARROW_TYPE_LARGE_BINARY:
      case NANOARROW_TYPE_FIXED_SIZE_BINARY:
      case NANOARROW_TYPE_BINARY_VIEW:
        return NANOARROW_TYPE_BINARY;
      /* Decimals come back as strings through the ODBC layer. */
      case NANOARROW_TYPE_DECIMAL128:
      case NANOARROW_TYPE_DECIMAL256:
        return NANOARROW_TYPE_STRING;
      default:
        return ingest_type;
    }
  }

  std::string catalog() const override { return "DB"; }
  std::string db_schema() const override { return "DBA"; }

  /* ----- Feature toggles (Phases 1-9 implementation status) ----- */

  bool supports_bulk_ingest_catalog() const override { return false; }
  bool supports_bulk_ingest_db_schema() const override { return true; }
  bool supports_bulk_ingest_temporary() const override { return true; }
  bool supports_cancel() const override { return true; }
  bool supports_concurrent_statements() const override { return false; }
  bool supports_execute_schema() const override { return true; }
  bool supports_get_option() const override { return true; }
  bool supports_partitioned_data() const override { return true; }
  bool supports_transactions() const override { return true; }
  bool supports_get_sql_info() const override { return true; }
  bool supports_get_objects() const override { return true; }
  bool supports_metadata_current_catalog() const override { return false; }
  bool supports_metadata_current_db_schema() const override { return false; }
  bool supports_dynamic_parameter_binding() const override { return true; }
  bool supports_rows_affected() const override { return true; }
  bool supports_statistics() const override { return true; }
  bool supports_error_on_incompatible_schema() const override { return true; }

  /* Ingest: View types (StringView, BinaryView) and Float16 are not
   * mapped by virt_arrow_to_virtuoso_ddl; skip those tests.            */
  bool supports_ingest_view_types() const override { return false; }
  bool supports_ingest_float16() const override { return false; }

  /* DDL in Virtuoso implicitly commits an open transaction. */
  bool ddl_implicit_commit_txn() const override { return true; }

  /* ----- Expected values for GetInfo ----- */
  std::optional<adbc_validation::SqlInfoValue> supports_get_sql_info(
      uint32_t info_code) const override {
    switch (info_code) {
      case ADBC_INFO_DRIVER_ADBC_VERSION:
        return ADBC_VERSION_1_1_0;
      case ADBC_INFO_DRIVER_NAME:
        return std::string("ADBC Driver for Virtuoso");
      case ADBC_INFO_DRIVER_VERSION:
        return std::string("0.0.1-dev");
      case ADBC_INFO_VENDOR_NAME:
        return std::string("OpenLink Software");
      default:
        return std::nullopt;
    }
  }
};

/* ====================================================================
 *  Test fixtures
 *
 *  Three fixture classes, one per ADBC handle, each pulling their
 *  quirks from VirtuosoQuirks. The ADBCV_TEST_* macros expand to
 *  the full GTest test suite.
 * ==================================================================== */

class VirtuosoDatabaseTest : public ::testing::Test,
                             public adbc_validation::DatabaseTest {
 public:
  const adbc_validation::DriverQuirks* quirks() const override {
    return &quirks_;
  }
  void SetUp() override { ASSERT_NO_FATAL_FAILURE(SetUpTest()); }
  void TearDown() override { ASSERT_NO_FATAL_FAILURE(TearDownTest()); }

 protected:
  VirtuosoQuirks quirks_;
};
ADBCV_TEST_DATABASE(VirtuosoDatabaseTest)

class VirtuosoConnectionTest : public ::testing::Test,
                               public adbc_validation::ConnectionTest {
 public:
  const adbc_validation::DriverQuirks* quirks() const override {
    return &quirks_;
  }
  void SetUp() override { ASSERT_NO_FATAL_FAILURE(SetUpTest()); }
  void TearDown() override { ASSERT_NO_FATAL_FAILURE(TearDownTest()); }

 protected:
  VirtuosoQuirks quirks_;
};
ADBCV_TEST_CONNECTION(VirtuosoConnectionTest)

class VirtuosoStatementTest : public ::testing::Test,
                              public adbc_validation::StatementTest {
 public:
  const adbc_validation::DriverQuirks* quirks() const override {
    return &quirks_;
  }
  void SetUp() override { ASSERT_NO_FATAL_FAILURE(SetUpTest()); }
  void TearDown() override { ASSERT_NO_FATAL_FAILURE(TearDownTest()); }

 protected:
  VirtuosoQuirks quirks_;
};
ADBCV_TEST_STATEMENT(VirtuosoStatementTest)

/* ====================================================================
 *  AdbcDriverInit compatibility test
 *
 *  Verifies the driver init entrypoint rejects unknown versions and
 *  that the 1.0.0-sized struct does not clobber 1.1.0 fields.
 * ==================================================================== */

namespace {

int Canary(const struct AdbcError*) { return 0; }

}  // namespace

TEST_F(VirtuosoDatabaseTest, AdbcDriverBackwardsCompatibility) {
  struct AdbcDriver driver;
  std::memset(&driver, 0, ADBC_DRIVER_1_1_0_SIZE);
  driver.ErrorGetDetailCount = Canary;

  ASSERT_THAT(AdbcDriverInit(ADBC_VERSION_1_0_0, &driver, &error),
              IsOkStatus(&error));
  ASSERT_EQ(Canary, driver.ErrorGetDetailCount);

  ASSERT_THAT(AdbcDriverInit(424242, &driver, &error),
              IsStatus(ADBC_STATUS_NOT_IMPLEMENTED, &error));
}
