#!/bin/sh -e

# Specify the Python interpreter to use.
# This is the command executed for the Python interpreter that is going
# to be included in KLayout.

python="python3.5m"

# Specify the Ruby interpreter to use.
# This is the command executed for the Ruby interpreter that is going
# to be included in KLayout.

ruby="ruby"

# Specify the path to the NSIS compiler

makensis=/c/Program\ Files\ \(x86\)/NSIS/makensis.exe

# ---------------------------------------------------
# General initialization

if ! [ -e ./build.sh ]; then
  echo "ERROR: build script not found (not in the main directory?)"
  exit 1
fi

# source the versions
. ./version.sh

pwd=$(pwd)

# ---------------------------------------------------
# Bootstrap script
# This branch will fork to the actual builds for win32 and win64

if [ "$KLAYOUT_BUILD_IN_PROGRESS" == "" ]; then

  self=$(which $0)

  export KLAYOUT_BUILD_IN_PROGRESS=1

  # Run ourself in MINGW32 system for the win32 build
  MSYSTEM=MINGW32 bash --login -c "cd $pwd ; $self"

  # Run ourself in MINGW64 system for the win64 build
  MSYSTEM=MINGW64 bash --login -c "cd $pwd ; $self"

  exit 0

fi

# ---------------------------------------------------
# Actual build branch

if [ "$MSYSTEM" == "MINGW32" ]; then
  arch=win32
  mingw_inst=/mingw32
elif [ "$MSYSTEM" == "MINGW64" ]; then
  arch=win64
  mingw_inst=/mingw64
else
  echo "ERROR: not in mingw32 or mingw64 system."
fi

target=$pwd/bin-release-$arch
build=$pwd/build-release-$arch
src=$pwd/src
scripts=$pwd/scripts

echo "------------------------------------------------------------------"
echo "Running build for architecture $arch .."
echo ""
echo "  target = $target"
echo "  build = $build"
echo "  version = $KLAYOUT_VERSION"
echo "  date = $KLAYOUT_VERSION_DATE"
echo "  rev = $KLAYOUT_VERSION_REV"
echo ""

# Force a minimum rebuild for the version update
touch src/klayout_main/version.h

rm -rf $target
./build.sh -python $python -ruby $ruby -bin $target -build $build -j2

if ! [ -e $target ]; then
  echo "ERROR: Target directory $target not found"
  exit 1
fi

if ! [ -e $target/klayout.exe ]; then
  echo "ERROR: Target directory $target does not contain klayout.exe"
  exit 1
fi

# ----------------------------------------------------------
# Image formats

echo "Installing image format plugins .."
cp -R $mingw_inst/share/qt5/plugins/imageformats $target
# remove the debug versions - otherwise they pull in the debug Qt libs
rm $target/imageformats/*d.dll

# ----------------------------------------------------------
# Ruby dependencies

rm -rf $target/.ruby-paths.txt
echo '# Builds the Python paths.' >$target/.ruby-paths.txt
echo '# KLayout will load the paths listed in this file into sys.path' >>$target/.ruby-paths.txt
echo '# unless $KLAYOUT_PYTHONHOME ist set.' >>$target/.ruby-paths.txt
echo '# Use KLayout EXPRESSIONS syntax to specify a list of file paths.' >>$target/.ruby-paths.txt
echo '[' >>$target/.ruby-paths.txt

first=1
rubys=$($ruby -e 'puts $:' | sort)
for p in $rubys; do 
  p=$(cygpath $p)
  if [[ $p == "$mingw_inst"* ]] && [ -e "$p" ]; then
    rp=${p/"$mingw_inst/"}
    if [ $first == "0" ]; then
      echo "," >>$target/.ruby-paths.txt
    fi
    first=0
    echo -n "  combine(inst_path, '$rp')" >>$target/.ruby-paths.txt
    echo "Copying Ruby installation partial $p -> $target/$rp .."
    rm -rf $target/$rp
    mkdir -p $target/$rp
    rmdir $target/$rp
    cp -vR $p $target/$rp | sed -u 's/.*/echo -n ./' | sh
    echo ""
  fi
done

echo '' >>$target/.ruby-paths.txt
echo ']' >>$target/.ruby-paths.txt

# ----------------------------------------------------------
# Python dependencies

rm -rf $target/.python-paths.txt
echo '# Builds the Python paths.' >$target/.python-paths.txt
echo '# KLayout will load the paths listed in this file into sys.path' >>$target/.python-paths.txt
echo '# unless $KLAYOUT_PYTHONHOME ist set.' >>$target/.python-paths.txt
echo '# Use KLayout EXPRESSIONS syntax to specify a list of file paths.' >>$target/.python-paths.txt
echo '[' >>$target/.python-paths.txt

first=1
pythons=$($python -c "import sys; print('\n'.join(sys.path))" | sort)
for p in $pythons; do 
  p=$(cygpath $p)
  if [[ $p == "$mingw_inst"* ]] && [ -e "$p" ]; then
    rp=${p/"$mingw_inst/"}
    if [ $first == "0" ]; then
      echo "," >>$target/.python-paths.txt
    fi
    first=0
    echo -n "  combine(inst_path, '$rp')" >>$target/.python-paths.txt
    echo "Copying Python installation partial $p -> $target/$rp .."
    rm -rf $target/$rp
    mkdir -p $target/$rp
    rmdir $target/$rp
    cp -vR $p $target/$rp | sed -u 's/.*/echo -n ./' | sh
    echo ""
  fi
done

echo '' >>$target/.python-paths.txt
echo ']' >>$target/.python-paths.txt

# ----------------------------------------------------------
# Binary dependencies

new_libs=$(find $target -name "*.dll" -or -name "*.so")

while [ "$new_libs" != "" ]; do

  echo "Analyzing dependencies of $new_libs .."

  # Analyze the dependencies of our components and add the corresponding libraries from $mingw_inst/bin
  libs=$(objdump -p $new_libs | grep "DLL Name:" | sort -u | sed 's/.*DLL Name: *//')
  new_libs=""

  for l in $libs; do
    if [ -e $mingw_inst/bin/$l ] && ! [ -e $target/$l ]; then
      echo "Copying binary installation partial $mingw_inst/bin/$l -> $target/$l .."
      cp $mingw_inst/bin/$l $target/$l
      new_libs="$new_libs $target/$l"
    fi  
  done

done

# ----------------------------------------------------------
# Run NSIS

# TODO: NSIS now supports /nocd with which we would no
# longer require the copy
cp $scripts/klayout-inst.nsis $target
cd $target
NSIS_VERSION=$KLAYOUT_VERSION NSIS_ARCH=$arch "$makensis" klayout-inst.nsis

# ----------------------------------------------------------
# Produce the .zip file

zipname="klayout-$KLAYOUT_VERSION-$arch"

echo "Making .zip file $zipname.zip .."

rm -rf $zipname $zipname.zip
mkdir $zipname
cp -Rv *.dll .*-paths.txt imageformats lib $zipname | sed -u 's/.*/echo -n ./' | sh
cp klayout.exe $zipname/klayout_app.exe
cp klayout.exe $zipname/klayout_vo_app.exe
echo ""

zip -r $zipname.zip $zipname
rm -rf $zipname

