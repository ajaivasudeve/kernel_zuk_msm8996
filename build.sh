#!/bin/bash
kernel_dir=$PWD
CCACHE=$(command -v ccache)
HOME=/home/ajaivasudeve
anykernel=$PWD/flasher
ZIMAGE=$kernel_dir/out/arch/arm64/boot/Image.gz-dtb
CONFIG_FILE="z2_plus_defconfig"
kernel_name="droopy-eas"
kernel_version="z2_plus"
zip_name="$kernel_name-$kernel_version-$(date +"%d-%m-%Y-%H-%M-%S").zip"
date="$(date +"%d-%m-%Y")"
export ARCH=arm64
export SUBARCH=arm64
NC='\033[0m'
RED='\033[0;31m'
LGR='\033[1;32m'

export CLANG_TRIPLE=aarch64-linux-gnu-
export CROSS_COMPILE=$HOME/kernel/toolchains/aarch64-linux-android-4.9/bin/aarch64-linux-android-
export CROSS_COMPILE_ARM32=$HOME/kernel/toolchains/arm-linux-androideabi-4.9/bin/arm-linux-androideabi-
export CLANG_TCHAIN=$HOME/kernel/toolchains/clang/clang-r365631c/bin/clang
export KBUILD_COMPILER_STRING="$(${CLANG_TCHAIN} --version | head -n 1 | perl -pe 's/\(http.*?\)//gs' | sed -e 's/  */ /g')"
export KBUILD_BUILD_USER="ajaivasudeve"
export KBUILD_BUILD_HOST="slowpoke"

cd $kernel_dir
    START=$(date +"%s")
    echo -e ${LGR} "############### Cleaning ################${NC}"
    make O=out clean 
    make O=out mrproper
    rm $anykernel/Image.gz-dtb
    rm -rf $ZIMAGE

    echo -e ${LGR} "############# Generating Defconfig ##############${NC}"
    make ARCH="arm64" O=out $CONFIG_FILE -j$(nproc --all)

    echo -e ${LGR} "############### Compiling kernel ################${NC}"
    make CC="${CCACHE} ${CLANG_TCHAIN}" O=out -j$(nproc --all)
    
    if [[ -f $ZIMAGE ]]; then
        mv -f $ZIMAGE $anykernel
        cd $anykernel
        find . -name "*.zip" -type f
        find . -name "*.zip" -type f -delete
        zip -r9 UPDATE-AnyKernel2.zip * -x README UPDATE-AnyKernel2.zip
        mv UPDATE-AnyKernel2.zip $zip_name
        if [ ! -d $HOME/kernel/builds/$date ]; then
        	mkdir $HOME/kernel/builds/$date
        fi	
        mv $anykernel/$zip_name $HOME/kernel/builds/$date/$zip_name
        chmod -R 0777 $HOME/kernel/builds
        END=$(date +"%s")
        DIFF=$(($END - $START))
        echo -e ${LGR} "#################################################"
        echo -e ${LGR} "############### Build competed in $(($DIFF / 60)) minute(s) and $(($DIFF % 60)) seconds! #################"
        echo -e ${LGR} "#################################################${NC}"
    else
        echo -e ${RED} "#################################################"
        echo -e ${RED} "# Build failed, check warnings/errors! #"
        echo -e ${RED} "#################################################${NC}"
    fi
cd ${kernel_dir}