#!/bin/bash

##########################################
# Author: rucsheng@gmail.com             #
# Help script to compile Percona server  #
##########################################

echo "========================================================="
echo "PLEASE change the path where marked as 'CHANGE ME' first!"
echo "========================================================="

basedir=/tmp/debug/ # CHANGE ME: path where will you debug the code
builddir=${basedir}/build 
mkdir -p ${builddir}
cd ${builddir}

mysqlprefix=${basedir}/binrun
mysqlsrcdir=/data/src # CHANGE ME: path of the source code
mkdir -p ${mysqlprefix}

export CompileTime=`date "+%Y%m%d-%H%M"`;

cmake ${mysqlsrcdir} -DCMAKE_BUILD_TYPE=debug -DWITH_TIANMA_STORAGE_ENGINE=1 -DENABLE_GCOV=0 -DWITH_UNIT_TESTS=0 -DDEFAULT_CHARSET=utf8 -DDEFAULT_COLLATION=utf8_general_ci -DMYSQL_TCP_PORT=4316 -DWITH_ARCHIVE_STORAGE_ENGINE=1  -DWITH_BLACKHOLE_STORAGE_ENGINE=0 -DWITH_PERFSCHEMA_STORAGE_ENGINE=0 -DWITH_INNOBASE_STORAGE_ENGINE=1 -DWITH_MYISAM_STORAGE_ENGINE=1 -DWITH_ROCKSDB_STORAGE_ENGINE=1 -DENABLED_LOCAL_INFILE=1 -DWITH_EXTRA_CHARSETS=all -DWITH_SEMISYNC_MASTER=ON -DWITH_SEMISYNC_SLAVE=ON -DCMAKE_INSTALL_PREFIX=${mysqlprefix} -DWITH_SSL=bundled -DCMAKE_C_FLAGS="-Wno-vla -Wno-return-type -Wno-attributes -Wno-error=unused-parameter" -DCMAKE_CXX_FLAGS="-Wno-vla -Wno-return-type -Wno-attributes -Wno-error=unused-parameter" -DWITHOUT_TOKUDB=1  -DWITH_NDBCLUSTER_STORAGE_ENGINE=OFF -DWITH_PROTOBUF=system -DWITH_ZLIB=bundled -DWITH_BOOST=/tmp/boost # CHNAGE ME: path of the boost_1_67_0 library


