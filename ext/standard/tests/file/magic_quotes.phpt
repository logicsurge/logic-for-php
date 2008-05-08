--TEST--
various magic quotes tests
--INI--
error_reporting=14335
--FILE--
<?php

$filename = dirname(__FILE__)."/magic_quotes.dat";

var_dump(get_magic_quotes_runtime());
var_dump(get_magic_quotes_runtime("blah"));

file_put_contents($filename, "some'content'here\"and}there");

var_dump(set_magic_quotes_runtime(true));
var_dump(get_magic_quotes_runtime());
var_dump(set_magic_quotes_runtime(-1));
var_dump(get_magic_quotes_runtime());

var_dump(file_get_contents($filename));

var_dump(set_magic_quotes_runtime(0));
var_dump(get_magic_quotes_runtime());

var_dump(file_get_contents($filename));

var_dump(set_magic_quotes_runtime(1));
var_dump(get_magic_quotes_runtime());

file_put_contents($filename, "some'content'here\"and}there");

var_dump(file_get_contents($filename));

var_dump(set_magic_quotes_runtime(0));
var_dump(set_magic_quotes_runtime());
var_dump(get_magic_quotes_runtime());

var_dump(file_get_contents($filename));


@unlink($filename);

echo "Done\n";
?>
--EXPECTF-- 
Deprecated: Function get_magic_quotes_runtime() is deprecated in %s on line %d
int(0)

Deprecated: Function get_magic_quotes_runtime() is deprecated in %s on line %d
int(0)

Deprecated: Function set_magic_quotes_runtime() is deprecated in %s on line %d
bool(true)

Deprecated: Function get_magic_quotes_runtime() is deprecated in %s on line %d
int(1)

Deprecated: Function set_magic_quotes_runtime() is deprecated in %s on line %d
bool(true)

Deprecated: Function get_magic_quotes_runtime() is deprecated in %s on line %d
int(1)
string(30) "some\'content\'here\"and}there"

Deprecated: Function set_magic_quotes_runtime() is deprecated in %s on line %d
bool(true)

Deprecated: Function get_magic_quotes_runtime() is deprecated in %s on line %d
int(0)
string(27) "some'content'here"and}there"

Deprecated: Function set_magic_quotes_runtime() is deprecated in %s on line %d
bool(true)

Deprecated: Function get_magic_quotes_runtime() is deprecated in %s on line %d
int(1)
string(30) "some\'content\'here\"and}there"

Deprecated: Function set_magic_quotes_runtime() is deprecated in %s on line %d
bool(true)

Deprecated: Function set_magic_quotes_runtime() is deprecated in %s on line %d
bool(false)

Deprecated: Function get_magic_quotes_runtime() is deprecated in %s on line %d
int(0)
string(27) "some'content'here"and}there"
Done
