set client_min_messages=ERROR;
drop server if exists kt_server cascade;
set client_min_messages=NOTICE;

create extension if not exists kt_fdw;
create server if not exists kyototycoon
       foreign data wrapper kt_fdw
       options( port '1978', host '127.0.0.1', timeout '1000');

create foreign table if not exists kt (key bytea, val bytea, flags text) server kyototycoon;
create user mapping for public server kyototycoon;
select * from kt;
select pg_sleep(1);
select * from kt;
