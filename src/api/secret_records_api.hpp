#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include <liquid/runtime.hpp>
#include <nlohmann/json.hpp>

#include "lake/api/capabilities.hpp"
#include "lake/control/secret_records.hpp"
#include "lake/control/secrets.hpp"
#include "lake/control/workspace_records.hpp"

namespace lake::api {

// Secrets in their mount's workspace Zetta (docs/api/secrets.md, lake#328-#332). The workspace is
// chosen once, when the mount is created, and recorded in the mount's content-free route; every
// later call names the mount, so it names the workspace, and nothing falls back to another
// workspace or to PostgreSQL. Every call goes through call_workspace, which checks the workspace's
// space and readiness.
//
// Each change of state is a conditional write: a new secret version or key version claims its
// number `if_absent` and then moves the entry or key forward `if_revision`; delete, undelete and
// destroy rewrite each version at the revision they read. A writer that finds a number already
// claimed by a put that never moved the entry completes that put first.
template <class T> using SecretTask = liquid::runtime::task<control::ZettaControlResult<T>>;

class MountSecrets {
public:
    // `not_found` at /mount_id when no route names the mount.
    static std::expected<MountSecrets, ApiError> open(const ApiContext& context,
                                                      const std::string& mount_id);
    // The mount of a lease, found through the lease's route; `not_found` at /lease_id when no
    // route names the lease.
    static std::expected<MountSecrets, ApiError> open_lease(const ApiContext& context,
                                                            const std::string& lease_id);

    const control::SecretMountRoute& route() const {
        return route_;
    }
    control::WorkspaceRecords& records() {
        return records_;
    }

    SecretTask<control::SecretMount> mount();

    SecretTask<control::SecretVersion> put(control::PutSecret input);
    SecretTask<control::SecretVersion> read(std::string name, std::uint64_t version);
    SecretTask<control::SecretMetadata> metadata(std::string name);
    SecretTask<control::SecretMetadata> change(std::string name, std::vector<std::uint64_t> versions,
                                               control::SecretVersionAction action);

    SecretTask<control::EncryptionKey> create_key(std::string name);
    SecretTask<std::vector<control::EncryptionKey>> list_keys(int limit);
    SecretTask<control::EncryptionKey> get_key(std::string name);
    SecretTask<control::EncryptionKey> rotate_key(std::string name);
    SecretTask<control::TransitValue> encrypt(std::string name, std::string plaintext);
    SecretTask<control::TransitValue> decrypt(std::string name, std::string ciphertext);
    SecretTask<control::TransitValue> rewrap(std::string name, std::string ciphertext);
    SecretTask<control::TransitValue> sign(std::string name, std::string message,
                                           std::uint64_t version);
    SecretTask<control::TransitVerification> verify(std::string name, std::string message,
                                                    std::string signature, std::uint64_t version);
    SecretTask<control::TransitValue> hmac(std::string name, std::string message,
                                           std::uint64_t version);
    SecretTask<control::DerivedEncryptionKey> derive(std::string parent_name, std::string name,
                                                     std::string context);

    // Dynamic leases (lake#362), records of this mount's workspace. A lease's route is written
    // first, so the expiry loop can always find a lease whose record exists. Renew, revoke and
    // the expiry loop each rewrite the record at the revision they read: a renewal is refused
    // once the lease is revoked or expired, and a revocation or expiry is never undone by a
    // renewal computed from an earlier copy.
    SecretTask<control::IssuedDynamicSecret> lease(std::string role, int lifetime_seconds);
    SecretTask<control::DynamicSecretLease> get_lease(std::string lease_id);
    SecretTask<control::DynamicSecretLease> renew_lease(std::string lease_id,
                                                        int lifetime_seconds);
    SecretTask<control::DynamicSecretLease> revoke_lease(std::string lease_id);

private:
    MountSecrets(control::WorkspaceRecords records, control::SecretMountRoute route,
                 control::Store* store, std::string instance_kek)
        : records_(std::move(records)), route_(std::move(route)), store_(store),
          instance_kek_(std::move(instance_kek)) {}

    control::WorkspaceRecords records_;
    control::SecretMountRoute route_;
    control::Store* store_ = nullptr;
    std::string instance_kek_;
};

// A new mount in `mount.workspace_id`, which Registry::call has already checked belongs to
// `mount.customer_id`: its name claim, data key and record in the workspace, then its route.
SecretTask<control::SecretMount> create_secret_mount(const ApiContext& context,
                                                     control::SecretMount mount);
// The mounts of one workspace (and, when given, one organization), newest first. Only mounts
// whose route names this workspace are answered.
SecretTask<std::vector<control::SecretMount>>
list_secret_mounts(const ApiContext& context, std::string customer_id, std::string workspace_id,
                   std::string organization_id, int limit);
// A new customer KEK, then every data key in each of the customer's workspaces rewrapped by it
// (conditionally, rereading on a conflict), then the previous KEK forgotten. A rotation an earlier
// call left unfinished (a workspace that was not ready) is finished first.
SecretTask<control::CustomerKekRotation> rotate_customer_kek(const ApiContext& context,
                                                             std::string customer_id);

// One data key record whose wrap is by a KEK version other than the current one, rewrapped by the
// current one at the revision read (rereading on a conflict). Answers whether it wrote.
SecretTask<bool> rewrap_secret_data_key(const ApiContext& context,
                                        control::WorkspaceRecords& records, std::string id);

// The raw key a data key record holds, unwrapped by the customer KEK version it names.
control::ZettaControlResult<std::string> unwrap_secret_data_key(const ApiContext& context,
                                                                const control::SecretDataKey& key);

// One pass of the dynamic-lease expiry loop (lake#362), run by the server's tick. It asks the
// control store's due index (secret_lease_routes) for the leases due now, so it reads only the
// workspaces that hold one, and settles each in its workspace with a conditional write: an expired
// lease is marked expired and its sealed value erased, a revoked one has its value erased, and a
// renewed one is given its new due instant. A workspace that is not ready keeps its leases claimed
// until the hold passes and is tried again; a deleted workspace's leases went with it.
struct SecretLeaseSweep {
    std::size_t claimed = 0;
    std::size_t expired = 0;     // marked expired by this pass
    std::size_t settled = 0;     // already revoked or expired; route settled
    std::size_t rescheduled = 0; // renewed since it was indexed
    std::size_t removed = 0;     // a route whose record was never written
    std::size_t failed = 0;      // left claimed, tried again after the hold
};
inline constexpr int secret_lease_claim_hold_seconds = 60;
liquid::runtime::task<SecretLeaseSweep> expire_secret_leases(const ApiContext& context,
                                                             int limit = 500);

// A record failure or store error as a lake error.
ApiResult secret_records_failure(const control::ZettaControlError& error);

} // namespace lake::api
