/*
 * Copyright © 2013 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authored by: Thomas Voß <thomas.voss@canonical.com>
 */

#include <core/trust/store.h>
#include <core/trust/impl/sqlite3/store.h>

#include <core/posix/this_process.h>

#include <sqlite3.h>
#include <xdg.h>

#include <cstring>
#include <iostream>
#include <sstream>
#include <mutex>

#include <sys/stat.h>
#include <sys/types.h>

namespace fs = boost::filesystem;

namespace core
{
namespace trust
{
namespace impl
{
namespace sqlite
{
// ensure_dirs_or_throw ensures that the directory p exists
// with the correct permissions, i.e., 0700.
fs::path ensure_dir_or_throw(const fs::path& p)
{
    // Ideally, we would use fs::create_directories but
    //   https://svn.boost.org/trac/boost/ticket/11179
    // Prevents us from doing so.
    static const mode_t owner_all = S_IRWXU;
    if (mkdir(p.string().c_str(), owner_all) != 0)
    {
        if (errno != EEXIST) throw std::system_error(errno, std::system_category());
    }

    return p;
}

std::pair<int, bool> is_error(int result)
{
    switch(result)
    {
    case SQLITE_OK:
    case SQLITE_DONE:
    case SQLITE_ROW:
        return std::make_pair(result, false);
        break;
    default:
        return std::make_pair(result, true);
    }
}

struct Database;

class PreparedStatement
{
public:
    enum class State
    {
        done = SQLITE_DONE,
        row = SQLITE_ROW
    };

    PreparedStatement() : db(nullptr), statement(nullptr)
    {
    }

    PreparedStatement(const PreparedStatement&) = delete;
    PreparedStatement(PreparedStatement&& rhs)
        : db(rhs.db),
          statement(rhs.statement)
    {
        rhs.db = nullptr;
        rhs.statement = nullptr;
    }

    virtual ~PreparedStatement()
    {
        sqlite3_finalize(statement);
    }

    PreparedStatement& operator=(PreparedStatement&& rhs)
    {
        if (statement)
        {
            sqlite3_finalize(statement);
        }

        db = rhs.db;
        statement = rhs.statement;

        rhs.db = nullptr;
        rhs.statement = nullptr;

        return *this;
    }

    PreparedStatement& operator=(const PreparedStatement&) = delete;
    bool operator==(const PreparedStatement&) const = delete;

    template<int index>
    void bind_null()
    {
        int result; bool error;
        std::tie(result, error) = is_error(sqlite3_bind_null(statement, index));

        if (error)
            throw std::runtime_error(sqlite3_errstr(result) + std::string(": ") + error_from_db());
    }

    template<int index>
    void bind_double(double d)
    {
        int result; bool error;
        std::tie(result, error) = is_error(sqlite3_bind_double(statement, index, d));

        if (error)
            throw std::runtime_error(sqlite3_errstr(result) + std::string(": ") + error_from_db());
    }

    template<int index>
    void bind_text(const std::string& s)
    {
        if (s.empty())
            return;

        auto memory = (char*)::malloc(s.size());
        ::memcpy(memory, s.c_str(), s.size());

        auto deleter = [](void* p) { if (p) ::free(p); };

        int result; bool error;
        std::tie(result, error) = is_error(sqlite3_bind_text(statement, index, memory, s.size(), deleter));

        if (error)
            throw std::runtime_error(sqlite3_errstr(result) + std::string(": ") + error_from_db());
    }

    template<int index>
    void bind_int(std::int32_t i)
    {
        int result; bool error;
        std::tie(result, error) = is_error(sqlite3_bind_int(statement, index, i));

        if (error)
            throw std::runtime_error(sqlite3_errstr(result) + std::string(": ") + error_from_db());
    }

    template<int index>
    void bind_int64(std::int64_t i)
    {
        int result; bool error;
        std::tie(result, error) = is_error(sqlite3_bind_int64(statement, index, i));

        if (error)
            throw std::runtime_error(sqlite3_errstr(result) + std::string(": ") + error_from_db());
    }

    template<int index>
    std::int32_t column_int()
    {
        return sqlite3_column_int(statement, index);
    }

    template<int index>
    std::int64_t column_int64()
    {
        return sqlite3_column_int64(statement, index);
    }

    template<int index>
    std::string column_text()
    {
        auto ptr = reinterpret_cast<const char*>(sqlite3_column_text(statement, index));
        return (ptr ? ptr : "");
    }

    void reset()
    {
        int result; bool error;
        std::tie(result, error) = is_error(sqlite3_reset(statement));

        if (error)
            throw std::runtime_error(sqlite3_errstr(result) + std::string(": ") + error_from_db());
    }

    void clear_bindings()
    {
        int result; bool error;
        std::tie(result, error) = is_error(sqlite3_clear_bindings(statement));

        if (error)
            throw std::runtime_error(sqlite3_errstr(result) + std::string(": ") + error_from_db());
    }

    State step()
    {
        int result; bool error;
        std::tie(result, error) = is_error(sqlite3_step (statement));

        if (error)
            throw std::runtime_error(sqlite3_errstr(result) + std::string(": ") + error_from_db());

        return static_cast<State>(result);
    }

protected:
    friend struct Database;

    PreparedStatement(Database* db, sqlite3_stmt* statement)
        : db(db),
          statement(statement)
    {
        if (!statement)
            throw std::runtime_error("Cannot construct prepared statement for null statement.");
    }

    std::string error_from_db() const;

    Database* db;
    sqlite3_stmt* statement;
};

template<typename Tag>
struct TaggedPreparedStatement : public PreparedStatement
{
    TaggedPreparedStatement() : PreparedStatement()
    {
    }

    TaggedPreparedStatement(Database* db, sqlite3_stmt* statement)
        : PreparedStatement(db, statement)
    {
    }
};

struct Database
{
    Database(const std::string& fn)
    {
        auto result = sqlite3_open(fn.c_str(), &db);
        if (result != SQLITE_OK)
        {
            std::stringstream ss;
            ss << "Problem opening database file " << fn << ": " << sqlite3_errstr(result);
            throw std::runtime_error(ss.str());
        };

        sqlite3_extended_result_codes(db, 1);
    }

    Database(const Database&) = delete;

    ~Database()
    {
        sqlite3_close(db);
    }

    Database& operator=(const Database&) = delete;
    bool operator==(const Database&) = delete;

    void set_version(std::int32_t version)
    {
        std::string pragma = "PRAGMA user_version=" + std::to_string(version) + ";";
        prepare_statement(pragma).step();
    }

    std::int32_t get_version()
    {
        auto stmt = prepare_statement("PRAGMA user_version;"); stmt.step();
        return stmt.column_int<0>();
    }

    PreparedStatement prepare_statement(const std::string& statement)
    {
        sqlite3_stmt* stmt = nullptr;
        int result; bool e;
        std::tie(result, e) = is_error(
                    sqlite3_prepare(
                        db,
                        statement.c_str(),
                        statement.size(),
                        &stmt,
                        nullptr));

        if (e)
            throw std::runtime_error(sqlite3_errstr(result) + std::string(": ") + error());

        return PreparedStatement(this, stmt);
    }

    template<typename Statement>
    TaggedPreparedStatement<Statement> prepare_tagged_statement()
    {
        sqlite3_stmt* stmt = nullptr;
        int result; bool e;
        std::tie(result, e) = is_error(
                    sqlite3_prepare(
                        db,
                        Statement::statement().c_str(),
                        Statement::statement().size(),
                        &stmt,
                        nullptr));

        if (e)
            throw std::runtime_error(sqlite3_errstr(result) + std::string(": ") + error());

        return TaggedPreparedStatement<Statement>(this, stmt);
    }

    std::string error() const
    {
        auto msg = sqlite3_errmsg(db);
        return std::string{(msg ? msg : "")};
    }

    sqlite3* db = nullptr;
};

std::string PreparedStatement::error_from_db() const
{
    return db->error();
}

// A store implementation persisting requests in an sqlite database.
struct Store
        : public core::trust::Store,
          public std::enable_shared_from_this<Store>
{
    // Our schema version constant.
    static constexpr const std::int32_t version{1};

    // Describes the table and its schema for storing requests.
    struct RequestsTable
    {
        RequestsTable() = delete;

        // The name of the table.
        static const std::string& name()
        {
            static const std::string s{"requests"};
            return s;
        }

        // Collects all columns, their names and indices.
        //
        // |-------------------------------------------------------------------------------------|
        // |  Id : int | ApplicationId : Text | Feature : int | Timestamp : int64 | Answer : int |
        // |-------------------------------------------------------------------------------------|
        struct Column
        {
            Column() = delete;

            struct Id
            {
                static const std::string& name()
                {
                    static const std::string s{"'Id'"};
                    return s;
                }

                static const int index = 0;
            };

            struct ApplicationId
            {
                static const std::string& name()
                {
                    static const std::string s{"'ApplicationId'"};
                    return s;
                }

                static const int index = 1;
            };

            struct Feature
            {
                static const std::string& name()
                {
                    static const std::string s{"'Feature'"};
                    return s;
                }

                static const int index = 2;
            };

            struct Timestamp
            {
                static const std::string& name()
                {
                    static const std::string s{"'Timestamp'"};
                    return s;
                }

                static const int index = 3;
            };

            struct Answer
            {
                static const std::string& name()
                {
                    static const std::string s{"'Answer'"};
                    return s;
                }

                static const int index = 4;
            };
        };
    };

    struct Statements
    {
        struct CreateDataTableIfNotExists
        {
            static const std::string& statement()
            {
                static const std::string s
                {
                    "CREATE TABLE IF NOT EXISTS " +
                    sqlite::Store::RequestsTable::name() + " (" +
                    sqlite::Store::RequestsTable::Column::Id::name() + " INTEGER PRIMARY KEY ASC, " +
                    sqlite::Store::RequestsTable::Column::ApplicationId::name() + " TEXT NOT NULL, " +
                    sqlite::Store::RequestsTable::Column::Feature::name() + " BIGINT, " +
                    sqlite::Store::RequestsTable::Column::Timestamp::name() + " BIGINT, " +
                    sqlite::Store::RequestsTable::Column::Answer::name() + " INTEGER);"
                };
                return s;
            }
        };       

        struct Delete
        {
            static const std::string& statement()
            {
                static const std::string s
                {
                    "DELETE FROM " + sqlite::Store::RequestsTable::name()
                };
                return s;
            }
        };

        struct Insert
        {
            static const std::string& statement()
            {
                static const std::string s
                {
                    "INSERT INTO " + Store::RequestsTable::name() + " ('ApplicationId','Feature','Timestamp','Answer') VALUES (?,?,?,?);"
                };
                return s;
            }
        };

        struct RemoveApplication
        {
            static const std::string& statement()
            {
                static const std::string s
                {
                    "DELETE FROM " + Store::RequestsTable::name() + " WHERE ApplicationId=?;"
                };
                return s;
            }

            struct Parameter
            {
                struct ApplicationId { static const int index = 1; };
            };
        };
    };

    // An implementation of the query interface for the SQLite-based store.
    struct Query : public core::trust::Store::Query
    {
        struct Statements
        {
            // Encodes the parameter-index for the prepared delete statement.
            struct Delete
            {
                static const std::string& statement()
                {
                    static const std::string s
                    {
                        "DELETE FROM " +
                        Store::RequestsTable::name() +
                        " WHERE Id=?;"
                    };

                    return s;
                }

                struct Parameter
                {
                    struct Id { static const int index = 1; };
                };
            };

            // Encodes the parameter-indices for the prepared select statement.
            struct Select
            {
                static const std::string& statement()
                {
                    static const std::string select
                    {
                        "SELECT * FROM " +
                        Store::RequestsTable::name() +
                        " WHERE ApplicationId=IFNULL(?,ApplicationId) AND"
                        " Feature=IFNULL(?,Feature) AND"
                        " (Timestamp BETWEEN IFNULL(?, Timestamp) AND IFNULL(?,Timestamp)) AND"
                        " Answer=IFNULL(?,Answer)"
                        " ORDER BY Timestamp DESC;"
                    };
                    return select;
                }

                struct Parameter
                {
                    struct ApplicationId { static const int index = 1; };
                    struct Feature { static const int index = ApplicationId::index + 1; };
                    struct Timestamp
                    {
                        struct LowerBound { static const int index = Feature::index + 1; };
                        struct UpperBound { static const int index = LowerBound::index + 1; };
                    };
                    struct Answer { static const int index = Timestamp::UpperBound::index + 1; };
                };
            };
        };

        // Constructs the query and associates it with its store.
        // That is: As long as a query is alive, the store is kept alive, too.
        Query(const std::shared_ptr<Store>& store) : d{store}
        {
            // A freshly constructed query finds all entries.
            all();
        }

        Status status() const
        {
            return d.status;
        }

        void for_application_id(const std::string& id)
        {
            d.select_statement.bind_text<
                Statements::Select::Parameter::ApplicationId::index
            >(id);
        }

        void for_feature(core::trust::Feature feature)
        {
            d.select_statement.bind_int64<
                Statements::Select::Parameter::Feature::index
            >(feature.value);
        }

        void for_interval(const Request::Timestamp& begin, const Request::Timestamp& end)
        {
            d.select_statement.bind_int64<
                Statements::Select::Parameter::Timestamp::LowerBound::index
            >(begin.time_since_epoch().count());

            d.select_statement.bind_int64<
                Statements::Select::Parameter::Timestamp::UpperBound::index
            >(end.time_since_epoch().count());
        }

        void for_answer(Request::Answer answer)
        {
            d.select_statement.bind_int<
                Statements::Select::Parameter::Answer::index
            >(static_cast<int>(answer));
        }

        void all()
        {
            d.select_statement.reset();
            d.select_statement.clear_bindings();
        }

        void execute()
        {
            d.select_statement.reset();
            auto result = d.select_statement.step();

            switch(result)
            {
            case TaggedPreparedStatement<Statements::Select>::State::done:
                d.status = Status::eor;
                break;
            case TaggedPreparedStatement<Statements::Select>::State::row:
                d.status = Status::has_more_results;
                break;
            }
        }

        void next()
        {
            auto result = d.select_statement.step();

            switch(result)
            {
            case TaggedPreparedStatement<Statements::Select>::State::done:
                d.status = Status::eor;
                break;
            case TaggedPreparedStatement<Statements::Select>::State::row:
                d.status = Status::has_more_results;
                break;
            }
        }

        void erase()
        {
            if (Status::eor == d.status)
                throw std::runtime_error("Cannot delete request as query points beyond the result set.");

            auto id = d.select_statement.column_int<Store::RequestsTable::Column::Id::index>();

            d.delete_statement.reset();
            d.delete_statement.bind_int<Statements::Delete::Parameter::Id::index>(id);
            d.delete_statement.step();

            next();
        }

        Request current()
        {
            switch(d.status)
            {
            case Status::error: throw Errors::QueryIsInErrorState{};
            case Status::eor: throw Errors::NoCurrentResult{};
            case Status::armed: throw Errors::NoCurrentResult{};
            default:
            {
                trust::Request request
                {
                    d.select_statement.column_text<Store::RequestsTable::Column::ApplicationId::index>(),
                    trust::Feature
                    {
                        static_cast<trust::Feature::IntegerType>(d.select_statement.column_int<Store::RequestsTable::Column::Feature::index>())
                    },
                    std::chrono::system_clock::time_point
                    {
                        std::chrono::system_clock::duration
                        {
                            d.select_statement.column_int64<Store::RequestsTable::Column::Timestamp::index>()
                        }
                    },
                    (Request::Answer)d.select_statement.column_int<Store::RequestsTable::Column::Answer::index>()
                };
                return request;
            }
            }

            throw std::runtime_error("Oops ... we should never reach here.");
        }

        struct Private
        {
            Private(const std::shared_ptr<Store>& store)
                : store(store),
                  delete_statement(store->db.prepare_tagged_statement<Statements::Delete>()),
                  select_statement(store->db.prepare_tagged_statement<Statements::Select>())
            {
            }

            ~Private()
            {
            }

            std::shared_ptr<Store> store;
            TaggedPreparedStatement<Statements::Delete> delete_statement;
            TaggedPreparedStatement<Statements::Select> select_statement;
            Status status = Status::armed;
            std::string error;
        } d;
    };

    Store(const std::string &service_name, xdg::BaseDirSpecification& spec);
    ~Store();

    // Handles upgrades to the underlying database if the schema changes.
    void upgrade(std::int32_t from_version);

    const char* error() const;

    // Creates the data table holding all requests if it not already exists.
    void create_data_table_if_not_exists();

    // From core::trust::Store
    void reset();
    void add(const Request& request);
    void remove_application(const std::string& id);
    std::shared_ptr<core::trust::Store::Query> query();

    std::mutex guard;
    Database db;
    TaggedPreparedStatement<Statements::CreateDataTableIfNotExists> create_data_table_statement;

    TaggedPreparedStatement<Statements::Delete> delete_statement;
    TaggedPreparedStatement<Statements::Insert> insert_statement;
    TaggedPreparedStatement<Statements::RemoveApplication> remove_application_statement;
};
}
}
}
}

namespace trust = core::trust;
namespace sqlite = core::trust::impl::sqlite;

sqlite::Store::Store(const std::string& service_name, xdg::BaseDirSpecification& spec)
    : db{(ensure_dir_or_throw(spec.data().home() / service_name) / "trust.db").string()},
      create_data_table_statement{db.prepare_tagged_statement<Statements::CreateDataTableIfNotExists>()}
{
    upgrade(db.get_version());

    delete_statement = db.prepare_tagged_statement<Statements::Delete>();
    insert_statement = db.prepare_tagged_statement<Statements::Insert>();
    remove_application_statement = db.prepare_tagged_statement<Statements::RemoveApplication>();
}

sqlite::Store::~Store()
{
}

void sqlite::Store::upgrade(std::int32_t from_version)
{
    switch (from_version)
    {
    case 0:
        create_data_table_if_not_exists();
        db.set_version(Store::version);
        break;
    default:
        break;
    }
}

void sqlite::Store::create_data_table_if_not_exists()
{
    create_data_table_statement.step();
}

void sqlite::Store::reset()
{
    std::lock_guard<std::mutex> lg(guard);
    try
    {
        delete_statement.reset();
        delete_statement.step();
    } catch(const std::runtime_error& e)
    {
        throw trust::Store::Errors::ErrorResettingStore{e.what()};
    }
}


void sqlite::Store::add(const trust::Request& request)
{
    std::lock_guard<std::mutex> lg(guard);

    insert_statement.reset();
    insert_statement.bind_text<sqlite::Store::RequestsTable::Column::ApplicationId::index>(request.from);
    insert_statement.bind_int<sqlite::Store::RequestsTable::Column::Feature::index>(request.feature.value);
    insert_statement.bind_int64<sqlite::Store::RequestsTable::Column::Timestamp::index>(request.when.time_since_epoch().count());
    insert_statement.bind_int<sqlite::Store::RequestsTable::Column::Answer::index>(static_cast<int>(request.answer));
    insert_statement.step();
}

void sqlite::Store::remove_application(const std::string& id)
{
    std::lock_guard<std::mutex> lg(guard);

    remove_application_statement.reset();
    remove_application_statement.bind_text<Statements::RemoveApplication::Parameter::ApplicationId::index>(id);
    remove_application_statement.step();
}

std::shared_ptr<trust::Store::Query> sqlite::Store::query()
{
    return std::shared_ptr<trust::Store::Query>{new sqlite::Store::Query{shared_from_this()}};
}

std::shared_ptr<core::trust::Store> core::trust::impl::sqlite::create_for_service(const std::string& name, xdg::BaseDirSpecification& spec)
{
    if (name.empty())
        throw core::trust::Errors::ServiceNameMustNotBeEmpty();

    return std::make_shared<sqlite::Store>(name, spec);
}

std::shared_ptr<core::trust::Store> core::trust::create_default_store(const std::string& service_name)
{
    return core::trust::impl::sqlite::create_for_service(service_name, *xdg::BaseDirSpecification::create());
}
