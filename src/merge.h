#ifndef LDP_MERGE_H
#define LDP_MERGE_H

#include <string>

#include "../etymoncpp/include/postgres.h"
#include "options.h"
#include "schema.h"

using namespace std;

void mergeTable(const Options& opt, Log* log, const TableSchema& table,
        etymon::OdbcEnv* odbc, etymon::OdbcDbc* dbc, const DBType& dbt);
void dropTable(const Options& opt, Log* log, const string& tableName,
        etymon::OdbcDbc* dbc);
void placeTable(const Options& opt, Log* log, const TableSchema& table,
        etymon::OdbcDbc* dbc);
void updateStatus(const Options& opt, const TableSchema& table,
        etymon::OdbcDbc* dbc);
//void dropOldTables(const Options& opt, Log* log, etymon::OdbcDbc* dbc);

//void mergeAll(const Options& opt, Schema* schema, etymon::Postgres* db);

#endif
