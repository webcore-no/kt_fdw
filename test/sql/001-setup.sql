set client_min_messages=ERROR;
drop server if exists kt_server cascade;
set client_min_messages=NOTICE;

create  server kt_server foreign data wrapper kt_fdw;
create foreign table kt_test (key text, value text, flag text) server kt_server;
create user mapping for public server kt_server;

drop server if exists kt_server cascade;

create  server kt_server foreign data wrapper kt_fdw;
create foreign table kt_test (key bytea, value bytea, flag text) server kt_server;
create user mapping for public server kt_server;
