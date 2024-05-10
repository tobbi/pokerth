FIND_PATH(MYSQL_INCLUDE_DIR mysql.h 
    PATHS 
    /opt/local/include
    $ENV{HOME}/build/include
    PATH_SUFFIXES
    mysql5/mysql
    mysql
    )

FIND_LIBRARY(MYSQL_LIBRARIES NAMES mysqlclient PATHS
    $ENV{HOME}/build/lib/mysql
    /opt/local/lib
    /opt/local/lib/mysql5/mysql
    /usr/lib64
    NO_DEFAULT_PATH
    )



if (MYSQL_INCLUDE_DIR AND MYSQL_LIBRARIES)
    set (MYSQL_FOUND true)
endif(MYSQL_INCLUDE_DIR AND MYSQL_LIBRARIES)


if (MYSQL_FOUND)
    if (NOT MYSQL_FIND_QUIETLY)
        message(STATUS "Found mysql: ${MYSQL_LIBRARIES}")
    endif(NOT MYSQL_FIND_QUIETLY)
else (MYSQL_FOUND)
   if (MYSQL_FIND_REQUIRED)
      message(FATAL_ERROR "Could not find fitsio")
    endif(MYSQL_FIND_REQUIRED)
endif(MYSQL_FOUND) 
