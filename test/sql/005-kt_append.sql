create server kt_server foreign data wrapper kt_fdw;
create foreign table kt_test (key text, value text, flag text) server kt_server;
create user mapping for public server kt_server;
delete from kt_test;
insert into kt_test values ('key1', 'foo');
select * from kt_test;
insert into kt_test values ('key1', 'bar', 'kt_append');
select * from kt_test;

