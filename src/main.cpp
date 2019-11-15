#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <curl/curl.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>

#include "../etymoncpp/include/postgres.h"
#include "config_json.h"
#include "extract.h"
#include "merge.h"
#include "options.h"
#include "stage_json.h"
#include "timer.h"
#include "util.h"

static const char* optionHelp =
"Usage:  ldp <command> <options>\n"
"  e.g.  ldp load --okapi folio --database ldp\n"
"Commands:\n"
"  load              - Load data into the LDP database\n"
"  help              - Display help information\n"
"Options:\n"
"  --okapi <name>    - Extract data from the Okapi instance <name>, which\n"
"                      refers to the name of an object under \"okapis\" in\n"
"                      the configuration file that describes connection\n"
"                      parameters for the instance\n"
"  --database <name> - Load data into the database <name>, which refers to\n"
"                      the name of an object under \"databases\" in the\n"
"                      configuration file that describes connection\n"
"                      parameters for the database\n"
"  --config <file>   - Specify the location of the configuration file,\n"
"                      overriding the LDPCONFIG environment variable\n"
"  --unsafe          - Enable functions used for testing/debugging\n"
"  --nossl           - Disable SSL in the database connection (unsafe)\n"
"  --savetemps       - Disable deletion of temporary files containing\n"
"                      extracted data (unsafe)\n"
"  --dir             - Load data from a directory instead of extracting\n"
"                      from Okapi (unsafe)\n"
"  --verbose, -v     - Enable verbose output\n"
"  --debug           - Enable extremely verbose debugging output\n";

void debugNoticeProcessor(void *arg, const char *message)
{
    const Options* opt = (const Options*) arg;
    print(Print::debug, *opt,
            string("database response: ") + string(message));
}

static void initDB(const Options& opt, etymon::Postgres* db)
{
    string sql;
    sql = "CREATE SCHEMA IF NOT EXISTS history;";
    printSQL(Print::debug, opt, sql);
    { etymon::PostgresResult result(db, sql); }

    sql = "CREATE SCHEMA IF NOT EXISTS local;";
    printSQL(Print::debug, opt, sql);
    { etymon::PostgresResult result(db, sql); }
}

static void updateDBPermissions(const Options& opt, etymon::Postgres* db)
{
    string sql;
    sql = "GRANT SELECT ON ALL TABLES IN SCHEMA public TO ldp;";
    printSQL(Print::debug, opt, sql);
    { etymon::PostgresResult result(db, sql); }

    sql = "GRANT USAGE ON SCHEMA history TO ldp;";
    printSQL(Print::debug, opt, sql);
    { etymon::PostgresResult result(db, sql); }

    sql = "GRANT SELECT ON ALL TABLES IN SCHEMA history TO ldp;";
    printSQL(Print::debug, opt, sql);
    { etymon::PostgresResult result(db, sql); }

    sql = "GRANT CREATE, USAGE ON SCHEMA local TO ldp;";
    printSQL(Print::debug, opt, sql);
    { etymon::PostgresResult result(db, sql); }
}

void makeTmpDir(const Options& opt, string* loaddir)
{
    *loaddir = opt.extractDir;
    string filename = "tmp_ldp_" + to_string(time(nullptr));
    etymon::join(loaddir, filename);
    print(Print::debug, opt,
            string("creating directory: ") + *loaddir);
    mkdir(loaddir->c_str(), S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP |
            S_IROTH | S_IXOTH);
}

const char* sslmode(bool nossl)
{
    return nossl ? "disable" : "require";
}

void runLoad(const Options& opt)
{
    //string ct;
    //getCurrentTime(&ct);
    //if (opt.verbose)
    //    fprintf(opt.err, "%s: start time: %s\n", opt.prog, ct.c_str());

    print(Print::verbose, opt, "testing database connection");
    { etymon::Postgres db(opt.databaseHost, opt.databasePort, opt.databaseUser,
            opt.databasePassword, opt.databaseName, sslmode(opt.nossl)); }

    Schema schema;
    Schema::MakeDefaultSchema(&schema);

    ExtractionFiles extractionFiles(opt);
    string loadDir;

    if (opt.loadFromDir != "") {

        if (opt.verbose)
            fprintf(opt.err, "%s: reading data from directory: %s\n",
                    opt.prog, opt.loadFromDir.c_str());
        loadDir = opt.loadFromDir;

    } else {

        extractionFiles.savetemps = opt.savetemps;

        if (opt.verbose)
            fprintf(opt.err, "%s: starting data extraction\n", opt.prog);
        Timer extractionTimer(opt);

        CURLcode cc = curl_global_init(CURL_GLOBAL_ALL);
        if (cc) {
            throw runtime_error(string("initializing curl: ") +
                    curl_easy_strerror(cc));
        }

        if (opt.verbose)
            fprintf(opt.err, "%s: logging in to Okapi service\n", opt.prog);

        string token;
        okapiLogin(opt, &token);

        makeTmpDir(opt, &loadDir);
        extractionFiles.dir = loadDir;

        extract(opt, schema, token, loadDir, &extractionFiles);
        if (opt.verbose)
            extractionTimer.print("total data extraction time");

    }

    print(Print::verbose, opt, "connecting to database");
    etymon::Postgres db(opt.databaseHost, opt.databasePort, opt.databaseUser,
            opt.databasePassword, opt.databaseName, sslmode(opt.nossl));
    PQsetNoticeProcessor(db.conn, debugNoticeProcessor, (void*) &opt);

    string sql;
    sql = "BEGIN ISOLATION LEVEL READ COMMITTED;";
    printSQL(Print::debug, opt, sql);
    { etymon::PostgresResult result(&db, sql); }

    if (opt.verbose)
        fprintf(opt.err, "%s: initializing database\n", opt.prog);
    initDB(opt, &db);

    if (opt.verbose)
        fprintf(opt.err, "%s: starting load to database\n", opt.prog);
    Timer loadTimer(opt);

    stageAll(opt, &schema, &db, loadDir);

    mergeAll(opt, &schema, &db);

    if (opt.verbose)
        fprintf(opt.err, "%s: updating database permissions\n", opt.prog);
    updateDBPermissions(opt, &db);

    fprintf(opt.err, "%s: committing changes\n", opt.prog);
    sql = "COMMIT;";
    printSQL(Print::debug, opt, sql);
    { etymon::PostgresResult result(&db, sql); }
    fprintf(opt.err, "%s: all changes committed\n", opt.prog);

    //vacuumAnalyzeAll();

    if (opt.verbose) {
        fprintf(opt.err, "%s: data loading complete\n", opt.prog);
        loadTimer.print("total load time");
    }

    //getCurrentTime(&ct);
    //if (opt.verbose)
    //    fprintf(opt.err, "%s: end time: %s\n", opt.prog, ct.c_str());

    curl_global_cleanup();  // Clean-up after curl_global_init().
    // TODO Wrap curl_global_init() in a class.
}

void fillOpt(const Config& config, const string& basePointer, const string& key,
        string* result)
{
    string p = basePointer;
    p += key;
    config.getRequired(p, result);
}

void fillOptions(const Config& config, Options* opt)
{
    if (opt->loadFromDir == "") {
        string okapi = "/okapis/";
        okapi += opt->okapi;
        okapi += "/";
        fillOpt(config, okapi, "url", &(opt->okapiURL));
        fillOpt(config, okapi, "tenant", &(opt->okapiTenant));
        fillOpt(config, okapi, "user", &(opt->okapiUser));
        fillOpt(config, okapi, "password", &(opt->okapiPassword));
        fillOpt(config, okapi, "extractDir", &(opt->extractDir));
    }

    string database = "/databases/";
    database += opt->database;
    database += "/";
    fillOpt(config, database, "database", &(opt->databaseName));
    fillOpt(config, database, "type", &(opt->databaseType));
    fillOpt(config, database, "host", &(opt->databaseHost));
    fillOpt(config, database, "port", &(opt->databasePort));
    fillOpt(config, database, "user", &(opt->databaseUser));
    fillOpt(config, database, "password", &(opt->databasePassword));
    opt->dbtype.setType(opt->databaseType);
}

void run(const etymon::CommandArgs& cargs)
{
    Options opt;

    if (evalopt(cargs, &opt) < 0)
        throw runtime_error("unable to parse command line options");

    if (cargs.argc < 2 || opt.command == "help") {
        printf("%s", optionHelp);
        return;
    }

    Config config(opt.config);
    fillOptions(config, &opt);

    if (opt.debug)
        debugOptions(opt);

    if (opt.command == "load") {
        Timer t(opt);
        runLoad(opt);
        if (opt.verbose)
            t.print("total time");
        return;
    }
}

int main(int argc, char* argv[])
{
    etymon::CommandArgs cargs(argc, argv);
    try {
        run(cargs);
    } catch (runtime_error& e) {
        fprintf(stderr, "ldp: error: %s\n", e.what());
        return 1;
    }
    return 0;
}

