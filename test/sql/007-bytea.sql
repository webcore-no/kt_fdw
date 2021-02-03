set client_min_messages=ERROR;
drop server if exists kt_server cascade;
set client_min_messages=NOTICE;


create server kt_server foreign data wrapper kt_fdw;
create foreign table kt_test (key bytea, value bytea, flag text) server kt_server;
create user mapping for public server kt_server;
delete from kt_test;
insert into kt_test values (E'\\xDEAD00BEEF'::bytea, E'\\xDEAD00BEEF'::bytea);
select * from kt_test;
insert into kt_test values (E'\\xDEAD00BEEF'::bytea, 'foo', 'kt_set');
select * from kt_test;
update kt_test set value = 'bar' where key = E'\\xDEAD00BEEF'::bytea;
select * from kt_test;
delete from kt_test where key = E'\\xDEAD00BEEF'::bytea;
select * from kt_test;
