--TEST--
imagecopy doen't copy alpha, palette to truecolor
--SKIPIF--
<?php
        if (!function_exists('imagecopy')) die("skip gd extension not available\n");
?>
--FILE--
<?php
$file = dirname(__FILE__) . '/bug38212.gd2';
$im1 = imagecreatetruecolor(10,100);
imagefill($im1, 0,0, 0xffffff);
imagegd2($im1, $file);
$im = imagecreatefromgd2part($file, 0,0, -25,10);
unlink($file);
?>
--EXPECTF--
Warning: imagecreatefromgd2part(): '%sbug38212.gd2' is not a valid GD2 file in %sbug38212.php on line %d
