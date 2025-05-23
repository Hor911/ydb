/* custom error:column name: value conflicted without correlation name it may be one of: Input.value, d.value*/
PRAGMA DisableSimpleColumns;
USE plato;

$data = (SELECT key as kk, subkey as sk, value || value as value FROM Input WHERE cast(key as uint32)/100 > 3);

--INSERT INTO Output
SELECT
  value, key -- value is conflicted between Input and d sources
FROM Input JOIN $data as d ON Input.subkey = d.kk
;
