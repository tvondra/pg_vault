-- add a key with ID, comment and key data
CREATE OR REPLACE FUNCTION pg_vault_add_key(id TEXT, key BYTEA, comment TEXT)
	RETURNS void
	AS 'MODULE_PATHNAME', 'add_key'
	LANGUAGE C;

-- removes a key with particular ID
CREATE OR REPLACE FUNCTION pg_vault_delete_key(id TEXT)
	RETURNS void
	AS 'MODULE_PATHNAME', 'delete_key'
	LANGUAGE C;

-- lookup of a key by ID (returns the key data as bytea)
CREATE OR REPLACE FUNCTION pg_vault_lookup(id TEXT)
	RETURNS bytea
	AS 'MODULE_PATHNAME', 'lookup_key'
	LANGUAGE C;

-- lists all the keys (without the key data)
CREATE OR REPLACE FUNCTION pg_vault_list_keys(OUT id TEXT, OUT length INT, OUT comment TEXT)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'list_keys'
	LANGUAGE C;

-- delete all the keys (overwrites the vault memory)
CREATE OR REPLACE FUNCTION pg_vault_delete_keys()
	RETURNS void
	AS 'MODULE_PATHNAME', 'delete_keys'
	LANGUAGE C;

CREATE OR REPLACE FUNCTION pg_vault_encrypt(data text, id text, options text) returns bytea AS '
	SELECT pgp_sym_encrypt($1, pg_vault_lookup($2)::text, $3);
' LANGUAGE SQL SECURITY DEFINER;

CREATE OR REPLACE FUNCTION pg_vault_encrypt_bytea(data bytea, id text, options text) returns bytea AS '
	SELECT pgp_sym_encrypt_bytea($1, pg_vault_lookup($2)::text, $3);
' LANGUAGE SQL SECURITY DEFINER;

CREATE OR REPLACE FUNCTION pg_vault_decrypt(data bytea, id text, options text) returns text AS '
	SELECT pgp_sym_decrypt($1, pg_vault_lookup($2)::text, $3);
' LANGUAGE SQL SECURITY DEFINER;

CREATE OR REPLACE FUNCTION pg_vault_decrypt_bytea(data bytea, id text, options text) returns bytea AS '
	SELECT pgp_sym_decrypt_bytea($1, pg_vault_lookup($2)::text, $3);
' LANGUAGE SQL SECURITY DEFINER;

REVOKE ALL ON FUNCTION pg_vault_add_key (TEXT, BYTEA, TEXT, BOOLEAN) FROM public;
REVOKE ALL ON FUNCTION pg_vault_delete_key (TEXT) FROM public;
REVOKE ALL ON FUNCTION pg_vault_lookup (TEXT) FROM public;
REVOKE ALL ON FUNCTION pg_vault_list_keys (OUT TEXT, OUT INT, OUT TEXT) FROM public;
REVOKE ALL ON FUNCTION pg_vault_delete_keys () FROM public;
