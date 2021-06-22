#!/bin/bash

set -e

PYTHON_SCRIPTS=deps/conda/bin
if [[ $OSTYPE == msys ]]; then
  PYTHON_SCRIPTS=deps/conda/bin/Scripts
fi

SCRIPT_DIR=$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )
WORKSPACE=${SCRIPT_DIR}/..
ABS_WORKSPACE=`dirname $SCRIPT_DIR`
build_type="release"

print_help() {
  echo "Builds the release branch and produce an egg to the targets directory "
  echo
  echo "Usage: ./make_egg.sh --version=[version] [remaining options]"
  echo
  echo "  --build_number=[version] The build number of the egg, e.g. 123. Or 123.gpu"
  echo
  echo "  --version=[version]      Synonym to build_number. Maintained for legacy reasons"
  echo
  echo "  --skip_test              Skip unit test and doc generation."
  echo
  echo "  --skip_cpp_test          Skip C++ tests"
  echo
  echo "  --skip_build             Skip the build process"
  echo
  echo "  --skip_doc               Skip the doc generation"
  echo 
  echo "  --debug                  Use debug build instead of release"
  echo
  echo "  --prod                   Enable production metrics for the egg"
  echo
  echo "  --toolchain=[toolchain]  Specify the toolchain for the configure"
  echo
  echo "  --num_procs=n            Specify the number of proceses to run in parallel"
  echo
  echo "Produce an local egg and skip test and doc generation"
  echo "Example: ./make_egg.sh --skip_test"
  exit 1
} # end of print help

# command flag options
# Parse command line configure flags ------------------------------------------
while [ $# -gt 0 ]
  do case $1 in
    --version=*)            BUILD_NUMBER=${1##--version=} ;;
    --build_number=*)       BUILD_NUMBER=${1##--build_number=} ;;
    --toolchain=*)          toolchain=${1##--toolchain=} ;;
    --num_procs=*)          NUM_PROCS=${1##--num_procs=} ;;
    --skip_test)            SKIP_TEST=1;;
    --skip_cpp_test)        SKIP_CPP_TEST=1;;
    --skip_build)           SKIP_BUILD=1;;
    --skip_doc)             SKIP_DOC=1;;
    --debug)                build_type="debug";;
    --prod)                 IS_PROD=1;;
    --help)                 print_help ;;
    *) unknown_option $1 ;;
  esac
  shift
done

set -x

if [[ -z "${BUILD_NUMBER}" ]]; then
  BUILD_NUMBER="0"
fi


if [[ ! -z "${toolchain}" ]]; then
  toolchain="--toolchain=${toolchain}"
else
  toolchain="--cleanup_if_invalid"
fi

if [[ -z "${NUM_PROCS}" ]]; then
  NUM_PROCS=4
else
  export OMP_NUM_THREADS=$NUM_PROCS
fi

TARGET_DIR=${WORKSPACE}/target
if [[ ! -d "${TARGET_DIR}" ]]; then
  mkdir ${TARGET_DIR}
fi

# Setup the build environment,
# Set PYTHONPATH, PYTHONHOME, PYTHON_EXECUTABLE, NOSETEST_EXECUTABLE
source ${WORKSPACE}/oss_local_scripts/python_env.sh $build_type

# Windows specific
archive_file_ext="tar.gz"
if [[ $OSTYPE == msys ]]; then
  # C++ tests are default skipped on windows due to annoying issues around
  # unable to find libstdc++.dll which are not so easily fixable
  SKIP_CPP_TEST=1
  archive_file_ext="zip"
  unset PYTHONHOME
fi


### Build the source with version number ###
build_source() {
  echo -e "\n\n\n================= Build ${BUILD_NUMBER} ================\n\n\n"
  # Configure
  cd ${WORKSPACE}
  ./configure ${toolchain}
  # Make clean
  cd ${WORKSPACE}/${build_type}/oss_src/unity/python
  make oss_clean_python
  cd ${WORKSPACE}/${build_type}/oss_src/unity
  # Make
  make -j${NUM_PROCS}

  if [[ -z $SKIP_CPP_TEST ]]; then
      cd ${WORKSPACE}/oss_test
      touch $(find . -name "*.cxx")
      cd ${WORKSPACE}/${build_type}/oss_test
      make -j${NUM_PROCS}
  fi
  echo -e "\n\n================= Done Build Source ================\n\n"
}

# Run all unit test
cpp_test() {
  echo -e "\n\n\n================= Running Unit Test ================\n\n\n"
  cd ${WORKSPACE}/${BUILD_TYPE}
  ${WORKSPACE}/oss_local_scripts/run_cpp_tests.py -j 1
}

# Run all unit test
unit_test() {
  echo -e "\n\n\n================= Running Unit Test ================\n\n\n"
  cd ${WORKSPACE}
  python -c 'import sframe; sframe.sys_util.test_pylambda_worker()'
  oss_local_scripts/run_python_test.sh ${build_type}
  echo -e "\n\n================= Done Unit Test ================\n\n"
}

mac_patch_rpath() {
  echo -e "\n\n\n================= Patching Mac RPath ================\n\n\n"
  # on mac we need to do a little work to fix up RPaths
  cd ${WORKSPACE}/${build_type}/oss_src/unity/python
  # - look for all files
  # - run 'file' on it
  # - look for binary files (shared libraries, executables)
  # - output is in the form [file]: type, so cut on ":", we just want the file
  flist=`find . -type f -not -path "*/CMakeFiles/*" -not -path "./dist/*" | xargs -L 1 file | grep x86_64 | cut -f 1 -d :` 

  # change libpython2.7 link to @rpath/libpython2.7
  # Empirically, it could be one of either
  for f in $flist; do
    install_name_tool -change libpython2.7.dylib @rpath/libpython2.7.dylib $f || true
    install_name_tool -change /System/Library/Frameworks/Python.framework/Versions/2.7/Python @rpath/libpython2.7.dylib $f || true
  done
  # We are generally going to be installed in
  # a place of the form
  # PREFIX/lib/python2.7/site-packages/[module]
  # libpython2.7 will probably in PREFIX/lib
  # So for instance if I am in
  # PREFIX/lib/python2.7/site-packages/sframe/unity_server
  # I need to add
  # ../../../ to the rpath (to make it to lib/)
  # But if I am in
  #
  # PREFIX/lib/python2.7/site-packages/sframe/something/somewhere
  # I need to add
  # ../../../../ to the rpath
  for f in $flist; do 
    # Lets explain the regexes
    # - The first regex removes "./"
    # - The second regex replaces the string "[something]/" with "../"
    #   (making sure something does not contain '/' characters)
    # - The 3rd regex removes the last "filename" bit
    #
    # Example:
    # Input: ./sframe/cython/cy_ipc.so 
    # After 1st regex: sframe/cython/cy_ipc.so 
    # After 2nd regex: ../../cy_ipc.so 
    # After 3rd regex: ../../
    relative=`echo $f | sed 's:\./::g' | sed 's:[^/]*/:../:g' | sed 's:[^/]*$::'`
    # Now we need ../../ to get to PREFIX/lib
    rpath="@loader_path/../../$relative"
    install_name_tool -add_rpath $rpath $f || true
  done
}

### Package the release folder into an egg tarball, and strip the binaries ###
package_egg() {
  if [[ $OSTYPE == darwin* ]]; then
    mac_patch_rpath
  fi
  echo -e "\n\n\n================= Packaging Egg  ================\n\n\n"
  cd ${WORKSPACE}/${build_type}/oss_src/unity/python
  # cleanup old builds
  rm -rf *.egg-info
  rm -rf dist

  # package with prod environment
  if [[ $IS_PROD ]]; then
      cp conf/prod.py sframe/util/graphlab_env.py
  else
      cp conf/qa.py sframe/util/graphlab_env.py
  fi

  # strip binaries
  if [[ ! $OSTYPE == darwin* ]]; then
    cd ${WORKSPACE}/${build_type}/oss_src/unity/python/sframe
    BINARY_LIST=`find . -type f -exec file {} \; | grep x86 | cut -d: -f 1`
    echo "Stripping binaries: $BINARY_LIST"
    for f in $BINARY_LIST; do
	strip -s $f;
    done
    cd ..
  fi

  cd ${WORKSPACE}/${build_type}/oss_src/unity/python
  rm -rf build
  dist_type="sdist"
  if [[ $OSTYPE == darwin* ]] || [[ $OSTYPE == msys ]]; then
    dist_type="bdist_wheel"
  fi
  VERSION_NUMBER=`python -c "import sframe; print sframe.version"`
  ${PYTHON_EXECUTABLE} setup.py ${dist_type} # This produced an egg/wheel starting with SFrame-${VERSION_NUMBER} under dist/
  
  cd ${WORKSPACE}

  EGG_PATH=${WORKSPACE}/${build_type}/oss_src/unity/python/dist/SFrame-${VERSION_NUMBER}.${archive_file_ext}
  if [[ $OSTYPE == darwin* ]];then
    EGG_PATH=`ls ${WORKSPACE}/${build_type}/oss_src/unity/python/dist/SFrame-${VERSION_NUMBER}*.whl`
    temp=`echo $EGG_PATH | perl -ne 'print m/(^.*-).*$/'`
    platform_tag="macosx_10_5_x86_64.macosx_10_6_intel.macosx_10_9_intel.macosx_10_9_x86_64.macosx_10_10_intel.macosx_10_10_x86_64.macosx_10_11_x86_64.macosx_10_11_intel"
    NEW_EGG_PATH=${temp}${platform_tag}".whl"
    mv ${EGG_PATH} ${NEW_EGG_PATH}
    EGG_PATH=${NEW_EGG_PATH}
  elif [[ $OSTYPE == msys ]]; then
    EGG_PATH=${WORKSPACE}/${build_type}/oss_src/unity/python/dist/SFrame-${VERSION_NUMBER}-cp27-none-win_amd64.whl
  fi

  # Install the egg and do a sanity check
  $PIP_EXECUTABLE install --force-reinstall --ignore-installed ${EGG_PATH}
  unset PYTHONPATH
  $PYTHON_EXECUTABLE -c "import sframe; sframe.SArray(range(100)).apply(lambda x: x)"

  # Done copy to the target directory
  cp $EGG_PATH ${TARGET_DIR}/
  echo -e "\n\n================= Done Packaging Egg  ================\n\n"
}


# Generate docs
generate_docs() {
  echo -e "\n\n\n================= Generating Docs ================\n\n\n"

  $PIP_EXECUTABLE install sphinx
  $PIP_EXECUTABLE install sphinx-bootstrap-theme
  $PIP_EXECUTABLE install numpydoc
  SPHINXBUILD=${WORKSPACE}/$PYTHON_SCRIPTS/sphinx-build

  cd ${WORKSPACE}/oss_src/unity/python/doc
  make clean SPHINXBUILD=${SPHINXBUILD}
  make html SPHINXBUILD=${SPHINXBUILD}
  tar -czvf ${TARGET_DIR}/sframe_python_sphinx_docs.${archive_file_ext} *
}

set_build_number() {
  # set the build number 
  cd ${WORKSPACE}/${BUILD_TYPE}/oss_src/unity/python/
  sed -i -e "s/'.*'#{{BUILD_NUMBER}}/'${BUILD_NUMBER}'#{{BUILD_NUMBER}}/g" sframe/version_info.py
}

# Here is the main function()
if [[ -z $SKIP_BUILD ]]; then
  build_source
fi


if [[ -z $SKIP_CPP_TEST ]]; then
  cpp_test 
fi

if [[ -z $SKIP_TEST ]]; then
  unit_test
fi

set_build_number

package_egg

if [[ -z $SKIP_DOC ]]; then
  generate_docs
fi


