--TEST--
Testing declare statement with several type values
--SKIPIF--
<?php
if (!extension_loaded("mbstring")) {
  die("skip Requires ext/mbstring");
}
?>
--INI--
zend.multibyte=1
--FILE--
<?php

declare(encoding = 1);
declare(encoding = 112313123213131232100);
declare(encoding = 'utf-8');
declare(encoding = M_PI);

print 'DONE';

?>
--EXPECTF--
Warning: Unsupported encoding [1] in %sdeclare_001.php on line %d

Warning: Unsupported encoding [1.1231312321313E+20] in %sdeclare_001.php on line %d

Parse error: syntax error, unexpected 'M_PI' (T_STRING), expecting integer number (T_LNUMBER) or floating-point number (T_DNUMBER) or quoted-string (T_CONSTANT_ENCAPSED_STRING) in %s/declare_001.php on line 6
