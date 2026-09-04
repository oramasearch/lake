-- lake control-plane schema.
--
-- SOURCE OF TRUTH: oramasearch/lake, db/schema.sql. The operator vendors a byte-identical copy at
-- internal/controller/assets/lake-schema.sql and applies it to each Lake's Postgres through a
-- PostgresSchema (one transaction, guarded on the `audit_events` table, versioned by content hash).
-- Keep the two files identical once the Phase 6 operator waiver ends; its version string then
-- changes exactly when this does. The waived operator copy intentionally remains on its prior
-- completed wave until its separate pull request lands.
--
-- Postgres 18. Applied with `psql --single-transaction`: no BEGIN/COMMIT here. Idempotent.

CREATE EXTENSION IF NOT EXISTS pgcrypto;

CREATE TABLE IF NOT EXISTS schema_migrations (
    version    text        PRIMARY KEY,
    applied_at timestamptz NOT NULL DEFAULT now()
);

CREATE OR REPLACE FUNCTION set_updated_at() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    NEW.updated_at := now();
    RETURN NEW;
END $$;

-- ---------------------------------------------------------------------------------------------
-- Identity: who. A user is a person, an organization is a group. Neither owns anything; the
-- tenant that owns everything is the customer below. Identity is TAAP: taap_id is the
-- key-derived identifier (taap:1:<alg>:<base32>) once the actor has a key; nullable until then.
-- ---------------------------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS users (
    id           uuid        PRIMARY KEY DEFAULT gen_random_uuid(),
    taap_id      text        UNIQUE,
    email        text,
    display_name text        NOT NULL DEFAULT '',
    created_at   timestamptz NOT NULL DEFAULT now(),
    updated_at   timestamptz NOT NULL DEFAULT now()
);
CREATE UNIQUE INDEX IF NOT EXISTS users_email_lower_key ON users (lower(email)) WHERE email IS NOT NULL;
DROP TRIGGER IF EXISTS users_set_updated_at ON users;
CREATE TRIGGER users_set_updated_at BEFORE UPDATE ON users FOR EACH ROW EXECUTE FUNCTION set_updated_at();

CREATE TABLE IF NOT EXISTS organizations (
    id           uuid        PRIMARY KEY DEFAULT gen_random_uuid(),
    taap_id      text        UNIQUE,
    display_name text        NOT NULL DEFAULT '',
    created_at   timestamptz NOT NULL DEFAULT now(),
    updated_at   timestamptz NOT NULL DEFAULT now()
);
DROP TRIGGER IF EXISTS organizations_set_updated_at ON organizations;
CREATE TRIGGER organizations_set_updated_at BEFORE UPDATE ON organizations FOR EACH ROW EXECUTE FUNCTION set_updated_at();

-- ---------------------------------------------------------------------------------------------
-- Tenant: the customer owns everything (repositories, artifacts, secrets, keys, usage carry
-- customer_id ON DELETE CASCADE). A customer is an individual or an organization, never both:
-- customer_type_check ties the type to exactly one non-null identity column, and the UNIQUE
-- identity columns give exactly one customer per user and per organization (NULLs don't collide).
--
-- name is the handle: one global namespace shared by individuals and organizations
-- (customer_name_unique). Lowercase letters, digits and inner hyphens only, 1-256 chars
-- (customer_name_format), so uniqueness is effectively case-insensitive and a handle can never
-- carry spaces or uppercase. No default: the application generates handles
-- ({adjective}-{noun}-{number}) and must pre-check uniqueness to answer 409, not 500.
-- ---------------------------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS customers (
    id              uuid         PRIMARY KEY DEFAULT gen_random_uuid(),
    customer_type   text         NOT NULL,
    user_id         uuid         UNIQUE REFERENCES users(id)         ON DELETE CASCADE,
    organization_id uuid         UNIQUE REFERENCES organizations(id) ON DELETE CASCADE,
    name            varchar(256) NOT NULL,
    created_at      timestamptz  NOT NULL DEFAULT now(),
    updated_at      timestamptz  NOT NULL DEFAULT now(),
    CONSTRAINT customer_type_check CHECK (
        (customer_type = 'individual'   AND user_id IS NOT NULL AND organization_id IS NULL) OR
        (customer_type = 'organization' AND organization_id IS NOT NULL AND user_id IS NULL)),
    CONSTRAINT customer_name_unique UNIQUE (name),
    CONSTRAINT customer_name_format CHECK (name ~ '^[a-z0-9]([a-z0-9-]{0,254}[a-z0-9])?$')
);
DROP TRIGGER IF EXISTS customers_set_updated_at ON customers;
CREATE TRIGGER customers_set_updated_at BEFORE UPDATE ON customers FOR EACH ROW EXECUTE FUNCTION set_updated_at();

INSERT INTO schema_migrations (version) VALUES ('0001_identity_and_customers') ON CONFLICT DO NOTHING;

-- ---------------------------------------------------------------------------------------------
-- Repositories (migration 0002): owned by a customer, the tenant, and named within it, so the
-- address is <customer handle>/<name>. name is a slug in the handle format; UNIQUE (customer_id,
-- name). visibility is private or public. Deleting the customer deletes its repositories.
-- ---------------------------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS repositories (
    id          uuid         PRIMARY KEY DEFAULT gen_random_uuid(),
    customer_id uuid         NOT NULL REFERENCES customers(id) ON DELETE CASCADE,
    name        varchar(256) NOT NULL,
    description text         NOT NULL DEFAULT '',
    visibility  text         NOT NULL DEFAULT 'private',
    created_at  timestamptz  NOT NULL DEFAULT now(),
    updated_at  timestamptz  NOT NULL DEFAULT now(),
    CONSTRAINT repository_visibility_check CHECK (visibility IN ('private', 'public')),
    CONSTRAINT repository_name_format CHECK (name ~ '^[a-z0-9]([a-z0-9-]{0,254}[a-z0-9])?$'),
    CONSTRAINT repository_name_unique UNIQUE (customer_id, name)
);
CREATE INDEX IF NOT EXISTS repositories_customer_id_idx ON repositories (customer_id);
DROP TRIGGER IF EXISTS repositories_set_updated_at ON repositories;
CREATE TRIGGER repositories_set_updated_at BEFORE UPDATE ON repositories FOR EACH ROW EXECUTE FUNCTION set_updated_at();

INSERT INTO schema_migrations (version) VALUES ('0002_repositories') ON CONFLICT DO NOTHING;

-- ---------------------------------------------------------------------------------------------
-- Audit (migration 0005): a signed, hash-linked instance stream. sequence is ordering only; the
-- signed predecessor digest makes a gap or reordered row detectable. The append-only trigger
-- protects against application-level rewrites. lake_identity stores all instance identities so
-- events signed before a rotation remain verifiable. A generated identity's wrapped_seed is
-- AES-256-GCM ciphertext under the customer-independent instance KEK; private seed material is
-- never returned by lake.
-- ---------------------------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS lake_identity (
    id                uuid        PRIMARY KEY,
    taap_id           text        NOT NULL UNIQUE,
    public_key        text        NOT NULL,
    wrapped_seed      text        NOT NULL,
    created_at        timestamptz NOT NULL,
    rotation_document jsonb,
    current           boolean     NOT NULL DEFAULT true
);
CREATE UNIQUE INDEX IF NOT EXISTS lake_identity_one_current_idx ON lake_identity (current) WHERE current;

CREATE TABLE IF NOT EXISTS audit_events (
    sequence        bigserial    PRIMARY KEY,
    id              uuid         NOT NULL UNIQUE,
    occurred_at     timestamptz  NOT NULL,
    customer_id     uuid         REFERENCES customers(id) ON DELETE RESTRICT,
    kind            text         NOT NULL,
    principal       text         NOT NULL,
    object          text         NOT NULL,
    action          text         NOT NULL,
    outcome         text         NOT NULL,
    request_id      uuid         NOT NULL,
    method          text,
    device          text,
    details         jsonb        NOT NULL DEFAULT '{}'::jsonb,
    previous_digest text,
    signer          text         NOT NULL REFERENCES lake_identity(taap_id) ON DELETE RESTRICT,
    signature       text         NOT NULL,
    digest          text         NOT NULL UNIQUE,
    CONSTRAINT audit_events_outcome_check CHECK (outcome IN ('success', 'failure', 'denied', 'revoked')),
    CONSTRAINT audit_events_digest_format CHECK (digest ~ '^[0-9a-f]{64}$'),
    CONSTRAINT audit_events_predecessor_format CHECK (previous_digest IS NULL OR previous_digest ~ '^[0-9a-f]{64}$')
);
CREATE INDEX IF NOT EXISTS audit_events_principal_occurred_at_idx ON audit_events (principal, occurred_at);
CREATE INDEX IF NOT EXISTS audit_events_object_occurred_at_idx ON audit_events (object, occurred_at);
CREATE INDEX IF NOT EXISTS audit_events_outcome_occurred_at_idx ON audit_events (outcome, occurred_at);
CREATE INDEX IF NOT EXISTS audit_events_customer_occurred_at_idx ON audit_events (customer_id, occurred_at);

CREATE OR REPLACE FUNCTION reject_audit_event_rewrite() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    RAISE EXCEPTION 'audit_events is append-only';
END $$;
DROP TRIGGER IF EXISTS audit_events_reject_update ON audit_events;
CREATE TRIGGER audit_events_reject_update BEFORE UPDATE ON audit_events FOR EACH ROW EXECUTE FUNCTION reject_audit_event_rewrite();
DROP TRIGGER IF EXISTS audit_events_reject_delete ON audit_events;
CREATE TRIGGER audit_events_reject_delete BEFORE DELETE ON audit_events FOR EACH ROW EXECUTE FUNCTION reject_audit_event_rewrite();

CREATE TABLE IF NOT EXISTS audit_retention (
    customer_id    uuid        PRIMARY KEY REFERENCES customers(id) ON DELETE CASCADE,
    retention_days integer     NOT NULL,
    updated_at     timestamptz NOT NULL DEFAULT now(),
    CONSTRAINT audit_retention_floor_check CHECK (retention_days >= 400)
);
DROP TRIGGER IF EXISTS audit_retention_set_updated_at ON audit_retention;
CREATE TRIGGER audit_retention_set_updated_at BEFORE UPDATE ON audit_retention FOR EACH ROW EXECUTE FUNCTION set_updated_at();

INSERT INTO schema_migrations (version) VALUES ('0005_audit') ON CONFLICT DO NOTHING;
