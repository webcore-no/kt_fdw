set client_min_messages=ERROR;
drop server if exists kt_server cascade;
set client_min_messages=NOTICE;

create server kt_server foreign data wrapper kt_fdw;
create foreign table kt_test (key text, value text, flag text) server kt_server;
create user mapping for public server kt_server;
delete from kt_test;

-- Set with no value in storage
insert into kt_test values ('key1', 'foo', 'kt_set');
select * from kt_test;
-- Set same value
insert into kt_test values ('key1', 'foo', 'kt_set');
select * from kt_test;
-- Set differnt value
insert into kt_test values ('key1', 'bar', 'kt_set');
select * from kt_test;

