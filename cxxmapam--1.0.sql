\echo Use "CREATE EXTENSION cxxmapam" to load this file. \quit

CREATE FUNCTION cxxmap_tableam_handler(internal)
RETURNS table_am_handler
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE ACCESS METHOD cxxmap TYPE TABLE HANDLER cxxmap_tableam_handler;
COMMENT ON ACCESS METHOD cxxmap IS 'table access method using C++ std::map';
