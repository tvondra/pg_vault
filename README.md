pg_vault
========
This PostgreSQL extension provides a simple in-memory vault for
encryption keys (used for example with [pgcrypto][pgcrypto]). The
goal of such vault is to perform cryptography without passing the
keys/passphrases to the SQL functions directly, as that means that
all manipulating the data need to know the actual key, the key may
leak to various logs (e.g. slow query logs) and some tasks are
considerably more difficult (e.g. rolling out a new key).


What pg_vault is (not)
----------------------
The idea of pg_vault is to provide a way to store keys in memory,
make the key management easier and limit the danger of leaking the
keys into logs. It's rather a PoC of the idea than a production
ready code, so (a) it may change in various ways and (b) it may
be broken or flawed in various ways.

The extension is not:

* _a replacement for pgcrypto_ - The extension does not implement
  any sort of crypto algorithmsm, it completely relies on pgcrypto
  in this respect.

* _a magical box protecting the keys_ - It's not a replacement for
  hardware appliances designed to manage keys (see [HSM][HSM]).
  All the data is stored in memory, and if someone can access that
  piece of memory (e.g. from a C extension in PostgreSQL), it's
  a game over.


Management API
--------------
Management API allows addition of new keys, fetching the keys, etc.
This must not be accessible to regular users, just to superusers.
It provides five functions, with hopefully clear names:

 * `pg_vault_add_key(id TEXT, key BYTEA, comment TEXT)`
 * `pg_vault_delete_key(id TEXT)`
 * `pg_vault_lookup(id TEXT)`
 * `pg_vault_list_keys(OUT id TEXT, OUT length INT, OUT comment TEXT)`
 * `pg_vault_delete_keys()`

As you can see from the signatures, the methods work with three
different parameters:

 * `id` - a value uniquely identifying a key
 * `key` - the passphrase (encoded as bytea)
 * `comment` - arbitrary description of the key

The `id` is user-defined, and the only requirement is it has to be
unique. It may be a random value (along the key ID used in PGP), but
a label describing the purpose of that particular key might be better.
For example 'user-info-key' for key used to encrypt info about users
and so on.


User API
--------
The user API allows performing encryption/decryption without
specifying the actual key, but using a unique key ID instead. So
instead of something like this:

    -- encrypt / decrypt
    SELECT pgp_sym_encrypt('... data ...', 'mypassphrase');
    SELECT pgp_sym_dencrypt('... data ...', 'mypassphrase');

it's possible to do this:

    -- at database startup
    SELECT pg_vault_add_key('id', 'mypassphrase');

    -- encrypt / decrypt
    SELECT pgp_sym_encrypt('... data ...', pg_vault_lookup('id'))
    SELECT pgp_sym_decrypt('... data ...', pg_vault_lookup('id'))

Of course, this only works if the user can call `pg_vault_lookup()`
directly, with arbitrary key IDs, so everyone can dump all the keys.
It's still an improvement (no keys leaking into the logs) it's
possible to improve this by providing a wrapper like this:

	CREATE FUNCTION pg_vault_encrypt(data text, id text, options text)
		RETURNS bytea AS '
		SELECT pgp_sym_encrypt($1, pg_vault_lookup($2)::text, $3);
	' LANGUAGE SQL SECURITY DEFINER;

and revoking all the rights on `pg_vault_lookup()` from public. No
direct key access, the user never sees the encryption key data. This
kind of wrappers is provided by the `pg_vault` extension, mapping
to the `pgp_sym_*` methods in [pgcrypto][pgcrypto]:

 * `pg_vault_encrypt` (`pgp_sym_encrypt`)
 * `pg_vault_encrypt_bytea` (`pgp_sym_encrypt_bytea`)
 * `pg_vault_decrypt` (`pgp_sym_decrypt`)
 * `pg_vault_decrypt_bytea` (`pgp_sym_decrypt_bytea`)

The user still can reference arbitrary keys, as there are no checks
(ownership of the key, etc.). That may be improved by a different
kind of wrapper, hardcoding the the key in the body

	CREATE FUNCTION decrypt_column_a(data text, options text)
		RETURNS bytea AS '
		SELECT pgp_sym_encrypt($1, pg_vault_lookup('id')::text, $2);
	' LANGUAGE SQL SECURITY DEFINER;

and of course, granting this only to the users authorized to access
that particular column.


Install
-------
Installing the extension is quite simple, especially if you're on 9.1.
In that case all you need to do is this:

    $ make install

and then (after connecting to the database)

    db=# CREATE EXTENSION pg_vault;

If you're on pre-9.1 version, you'll have to do the second part manually
by running the SQL script (pg_vault--x.y.sql) in the database. If
needed, replace MODULE_PATHNAME by $libdir.


Config
------
No the functions are created, but you still need to load the shared
module. This needs to be done from postgresql.conf, as the module
needs to allocate space in the shared memory segment. So add this to
the config file (or update the current values)

    # libraries to load
    shared_preload_libraries = 'pg_vault'

    # known GUC prefixes
    custom_variable_classes = 'pg_vault'

    # config of the shared memory (default: 1MB)
    pg_vault.max_size = 2MB

Yes, there's a single GUC variable that defines the maximum size of
the shared segment. This is a hard limit, the shared segment is not
extensible and you need to set it so that all the dictionaries fit
into it and not much memory is wasted. The default size (1MB) is enough
for ~740 keys, which should be sufficient for most cases.

At this moment, all the keys are 'global' - shared by all the databases
in a cluster. Implementing per-database keys should not be difficult.

Also, this extension does not implement any kind of startup/shutdown
for the vault - whenever you start the database, you have to load
all the keys on your own. It might be possible to keep some sort of
wallet (encrypted file and a means to load it after startup), but
that's not implemented yet.


Possible improvements
---------------------
There are several possible improvements of the current version:

* _per-db keys_ - At this moment, all the keys are 'global' i.e.
  shared by all the databases in a cluster. This makes it rather
  difficult to use on clusters with multiple databases, as each
  database may access and manipulate all the other keys. It's
  however possible to fix this by tracking ID of the databases
  for each key (and only allowing access to the right keys).

  It's worth noting that this only improves the API - the memory
  is just as vulnerable as before (e.g. an extension written in
  C can still read the vault from shared memory and just ignore
  the database IDs).

* _dump/load the keys_ - Currently the keys are stored in memory
  in a raw form - completely unprotected. It might be possible
  to add some sort of PIN code (different for each DB / key),
  possibly improving the defense when someone reads the memory.
  OTOH the PIN needs to be provided somehow, may leak into logs
  and so on. Also, if you hardcode the PIN into a wrapper, the
  attacker may just dump the function definition, etc.

* _external storage_ - Instead of storing the vault in a shared
  memory segment, making it vulnerable to simply reading the memory
  from the backend process (C extensions are just shared libraries
  linked into the process), it might be stored separately and
  only accessible through a limited API, thus significantly
  reducing the attack surface. It might be a separate process on
  the same box (address space separate from the backend process),
  a different machine (accessed through network) or even something
  like [usbarmory][http://inversepath.com/usbarmory].

* _public crypto_ - All the examples in this README used symmetric
  crypto only. I don't think I've ever seen assymetric crypto done
  in a database, except maybe the operations using public keys. And
  there's not much need to protect those, because they're public.
  It however should not be that difficult to make it work with
  private keys too, if needed.


[HSM]: http://en.wikipedia.org/wiki/Hardware_security_module
[pgcrypto]: http://www.postgresql.org/docs/devel/static/pgcrypto.html
