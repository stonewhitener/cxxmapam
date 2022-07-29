CREATE EXTENSION cxxmapam;
CREATE TABLE cxxmap_tab (foo int, bar "char") USING cxxmap;
SELECT * FROM cxxmap_tab;
INSERT INTO cxxmap_tab VALUES (123, 'A');
SELECT * FROM cxxmap_tab;
INSERT INTO cxxmap_tab VALUES (456, 'B');
SELECT * FROM cxxmap_tab;

-- Clean up
DROP TABLE cxxmap_tab;
