#include "kernel/capabilities/bootstrap.hpp"
#include "kernel/logging.hpp"

#include <array>
#include <json/value.h>
#include <libpq-fe.h>
#include <memory>
#include <spdlog/spdlog.h>
#include <sstream>
#include <stdexcept>
#include <string>

namespace plinth::capabilities {

namespace {

struct KernelCap {
  const char* nspace;
  int version;
  const char* fn;
  const char* rbac_rule;
  const char* description;
};

// ICD-0.2.0 §Bootstrap: Kernel Capabilities — the minimum required set.
// Additional kernel capabilities will be appended here as kernel
// subsystems come online (realtime, storage, scheduler, …).
constexpr std::array KERNEL_CAPS = {
    KernelCap{.nspace = "kernel",
              .version = 1,
              .fn = "db.query",
              .rbac_rule = "kernel.db.query",
              .description = "Execute a read query against the database"},
    KernelCap{.nspace = "kernel",
              .version = 1,
              .fn = "db.exec",
              .rbac_rule = "kernel.db.exec",
              .description = "Execute a write statement against the database"},
    KernelCap{.nspace = "kernel",
              .version = 1,
              .fn = "log",
              .rbac_rule = "kernel.log",
              .description = "Write to the application log"},
    KernelCap{.nspace = "kernel",
              .version = 1,
              .fn = "audit",
              .rbac_rule = "kernel.audit",
              .description = "Write to the audit log"},
    KernelCap{.nspace = "kernel",
              .version = 1,
              .fn = "config.get",
              .rbac_rule = "kernel.config.get",
              .description = "Read kernel configuration values"},
};

auto build_conninfo(const Config::Database& db) -> std::string {
  std::ostringstream ss;
  ss << "host=" << db.host << " port=" << db.port << " dbname=" << db.database
     << " user=" << db.user << " password=" << db.password;
  return ss.str();
}

// Returns true if this is the first time the given rule is being
// registered in this DB (i.e. the INSERT affected one row). Mirrors
// the style used in groups::bootstrap_groups for kernel.admin so we
// can emit rbac.rule_registered exactly once across restarts.
auto insert_rule_if_absent(PGconn* conn, const KernelCap& cap) -> bool {
  std::array<const char*, 4> values = {
      cap.rbac_rule, cap.nspace, cap.description,
      "kernel", // extension_name for kernel-owned rules
  };
  std::unique_ptr<PGresult, decltype(&PQclear)> res(
      PQexecParams(conn,
                   "INSERT INTO plinth.rbac_rules "
                   "(rule, namespace, description, extension_name) "
                   "VALUES ($1, $2, $3, $4) "
                   "ON CONFLICT (rule) DO NOTHING",
                   4, nullptr, values.data(), nullptr, nullptr, 0),
      PQclear);
  if (PQresultStatus(res.get()) != PGRES_COMMAND_OK) {
    throw std::runtime_error(
        std::string("bootstrap_kernel_capabilities rule INSERT failed: ") +
        PQresultErrorMessage(res.get()));
  }
  const char* affected = PQcmdTuples(res.get());
  return affected != nullptr && std::string{affected} == "1";
}

// Returns true if this is the first time the given capability is being
// registered (INSERT affected one row).
auto insert_capability_if_absent(PGconn* conn, const KernelCap& cap,
                                 const std::string& signature) -> bool {
  auto version_str = std::to_string(cap.version);
  std::array<const char*, 7> values = {
      cap.nspace,
      version_str.c_str(),
      cap.fn,
      signature.c_str(),
      // provider_type = 'kernel'
      "kernel",
      cap.description,
      cap.rbac_rule,
  };
  std::unique_ptr<PGresult, decltype(&PQclear)> res(
      PQexecParams(
          conn,
          "INSERT INTO plinth.capabilities "
          "(namespace, version, function, signature, provider_type, "
          " extension_name, scope, description, rbac_rule) "
          "VALUES ($1, $2::int, $3, $4, $5, NULL, 'instance', $6, $7) "
          "ON CONFLICT (namespace, version, function, scope) DO NOTHING",
          7, nullptr, values.data(), nullptr, nullptr, 0),
      PQclear);
  if (PQresultStatus(res.get()) != PGRES_COMMAND_OK) {
    throw std::runtime_error(
        std::string("bootstrap_kernel_capabilities INSERT failed: ") +
        PQresultErrorMessage(res.get()));
  }
  const char* affected = PQcmdTuples(res.get());
  return affected != nullptr && std::string{affected} == "1";
}

} // namespace

auto bootstrap_kernel_capabilities(const Config::Database& db_cfg) -> void {
  auto conninfo = build_conninfo(db_cfg);
  PGconn* conn = PQconnectdb(conninfo.c_str());
  if (PQstatus(conn) != CONNECTION_OK) {
    std::string err = PQerrorMessage(conn);
    PQfinish(conn);
    throw std::runtime_error(
        "bootstrap_kernel_capabilities: PG connect failed: " + err);
  }
  std::unique_ptr<PGconn, decltype(&PQfinish)> guard(conn, PQfinish);

  spdlog::info("seeding kernel capabilities and their RBAC rules");

  for (const auto& cap : KERNEL_CAPS) {
    auto signature = std::string{cap.nspace} + ":" +
                     std::to_string(cap.version) + ":" + std::string{cap.fn};

    auto rule_is_new = insert_rule_if_absent(conn, cap);
    if (rule_is_new) {
      Json::Value detail(Json::objectValue);
      detail["rule"] = std::string{cap.rbac_rule};
      detail["namespace"] = std::string{cap.nspace};
      detail["extension_name"] = "kernel";
      plinth::log::audit_sync(db_cfg, "rbac.rule_registered", detail);
    }

    auto cap_is_new = insert_capability_if_absent(conn, cap, signature);
    if (cap_is_new) {
      Json::Value detail(Json::objectValue);
      detail["signature"] = signature;
      detail["provider_type"] = "kernel";
      detail["extension_name"] = Json::nullValue;
      detail["scope"] = "instance";
      detail["rbac_rule"] = std::string{cap.rbac_rule};
      plinth::log::audit_sync(db_cfg, "capability.registered", detail);
    }
  }

  spdlog::info("kernel capabilities seeded");
}

} // namespace plinth::capabilities
