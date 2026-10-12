/* gaussdb-package-local/extensions/extopt/extopt--1.0.sql */

-- complain if script is sourced in gsql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION extopt" to load this file. \quit

CREATE SCHEMA extopt;

CREATE FUNCTION extopt.partial_capability(execution_id bigint)
RETURNS boolean
AS 'MODULE_PATHNAME', 'extopt_partial_capability'
LANGUAGE C STRICT VOLATILE NOT FENCED;

CREATE FUNCTION extopt.query_source(sql text,
    OUT query_payload text,
    OUT query_catalog text)
RETURNS record
AS 'MODULE_PATHNAME', 'extopt_query_source'
LANGUAGE C STRICT NOT FENCED;

CREATE FUNCTION extopt._btree_tree_height(index_oid oid)
RETURNS int4
AS 'MODULE_PATHNAME', 'extopt_btree_tree_height'
LANGUAGE C STRICT NOT FENCED;

REVOKE ALL ON FUNCTION extopt._btree_tree_height(oid) FROM PUBLIC;

CREATE FUNCTION extopt.btree_storage_metadata(index_oid oid,
    OUT relpages int4,
    OUT tree_height int4)
RETURNS record
LANGUAGE SQL SECURITY DEFINER STRICT
SET search_path = pg_catalog
AS $$
SELECT CASE WHEN EXISTS (
           SELECT 1 FROM pg_catalog.pg_partition p
           WHERE p.oid = index_oid AND p.parttype = 'x')
       THEN (SELECT p.relpages::int4 FROM pg_catalog.pg_partition p
             WHERE p.oid = index_oid AND p.parttype = 'x')
       ELSE (pg_catalog.pg_relation_size(index_oid::regclass) /
             pg_catalog.current_setting('block_size')::int)::int4 END,
       extopt._btree_tree_height(index_oid)
$$;
