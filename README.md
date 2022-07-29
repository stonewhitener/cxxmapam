# cxxmapam

Table access method for PostgreSQL using C++ `std::map` (educational purposes).

- Only support simple `INSERT` / `SELECT` queries
- All inserting values must be non-null and pass-by-value type
- No index support
- No transaction support
- No logging and checkpointing

## Requirements

- PostgreSQL 14 or 15
- GNU Make
- C++ 20 compiler (recent versions of GCC/Clang are recommended)

## Install

```sh
git clone https://github.com/stonewhitener/cxxmapam.git
cd cxxmapam
make USE_PGXS=1
make install USE_PGXS=1
```

## Usage

```
=# CREATE EXTENSION cxxmapam;
CREATE EXTENSION
=# CREATE TABLE cxxmap_tab (foo int, bar "char") USING cxxmap;
CREATE TABLE
=# INSERT INTO cxxmap_tab VALUES (123, 'A');
INSERT 0 1
=# INSERT INTO cxxmap_tab VALUES (456, 'B');
INSERT 0 1
=# SELECT * FROM cxxmap_tab;
 foo | bar 
-----+-----
 123 | A
 456 | B
(2 rows)

=# DROP TABLE cxxmap_tab;
DROP TABLE
=# DROP EXTENSION cxxmapam;
DROP EXTENSION
```
