set client_min_messages=ERROR;
drop server if exists kt_server cascade;
set client_min_messages=NOTICE;


create server kt_server foreign data wrapper kt_fdw
options(host '127.0.0.1', port '1978', timeout '-1', reconnect '1000');
create foreign table kt_test (key bytea, value bytea, flag text) server kt_server;
create user mapping for public server kt_server;
alter server kt_server options(set host '127.0.0.1', set port '1978', set timeout '-1', set reconnect '1000');
