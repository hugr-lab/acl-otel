-- spec 018 e2e: a node with acl + acl_otel defines, grants, attaches and writes; Marquez receives it
LOAD '__ACL__';
LOAD '__OTEL__';
ATTACH ':memory:' AS store;
ATTACH ':memory:' AS phys;
CREATE TABLE phys.main.orders(id INTEGER, amount DOUBLE, ssn VARCHAR, tenant VARCHAR,
    address STRUCT(city VARCHAR, zip VARCHAR));
CREATE TABLE phys.main.sink(id INTEGER, total DOUBLE);
SELECT acl_use_db('store', 'acl', true);
SET GLOBAL acl_allow_anonymous_admin = true;
SET GLOBAL acl_lineage_level = 'on';
SET GLOBAL acl_lineage_namespace = 'acl://e2e';
SET GLOBAL acl_otel_lineage = '__URL__';
ACL ADMIN CREATE VIRTUAL CATALOG sales;
ACL ADMIN CREATE VIRTUAL TABLE sales.orders AS phys.main.orders COLUMNS (id, amount, ssn = NULL, address);
ACL ADMIN CREATE VIRTUAL VIEW sales.big AS SELECT id, amount * 2 AS doubled FROM phys.main.orders WHERE amount > 10;
ACL ADMIN CREATE VIRTUAL TABLE sales.sink AS phys.main.sink;
ACL ADMIN CREATE ROLE clerk;
ACL ADMIN GRANT CATALOG sales TO ROLE clerk WITH (select, insert) MAIN;
ACL ADMIN GRANT TABLE sales.orders TO ROLE clerk COLUMNS (id, amount, address.city);
ATTACH ':memory:' AS warehouse;
ACL ROLE "clerk" LINEAGE PARENT 'airflow/daily.load/01929e3a-0000-7000-8000-000000000001' JOB 'dbt.sink_model'
    INSERT INTO sink SELECT id, amount FROM orders WHERE amount > 0;
SELECT acl_lineage_flush() AS node_flushed;
SELECT acl_otel_lineage_flush() AS otel_flushed;
SELECT acl_otel_status()::JSON->'lineage' AS lineage_status;
