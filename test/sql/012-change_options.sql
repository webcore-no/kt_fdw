set client_min_messages=ERROR;
drop server if exists kt_server cascade;
set client_min_messages=NOTICE;


create server kt_server foreign data wrapper kt_fdw
options(host 'no_hostname', port '42', timeout '-1', reconnect '1');
create foreign table kt_test (key bytea, value bytea, flag text) server kt_server;
create user mapping for public server kt_server;
delete from kt_test;
select pg_sleep(.1);
select * from kt_test;


select pg_sleep(.1);
alter server kt_server options( set host '127.0.0.1');
delete from kt_test;
select pg_sleep(.1);
select * from kt_test;


select pg_sleep(.1);
alter server kt_server options( set port '1978');
delete from kt_test;
select pg_sleep(.1);
select * from kt_test;

select pg_sleep(.1);
alter server kt_server options( set host 'localhouse');
delete from kt_test;
select pg_sleep(.1);
select * from kt_test;

