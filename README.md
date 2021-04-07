# IntelMediaSDKSample
A simple sample for intel media sdk <br>
Only for linux 

## Requirements

1. Intel media sdk (https://github.com/Intel-Media-SDK/MediaSDK/releases) <br>
Default installation path : /opt/intel 

2. Linux 4.18.20 kernel or later

```
echo "download 4.18.20 kernel"
if [ ! -f ./linux-4.18.20.tar.xz ]; then
wget https://www.kernel.org/pub/linux/kernel/v4.x/linux-4.18.20.tar.xz
fi
tar -xJf linux-4.18.20.tar.xz
cd linux-4.18.20
echo "build 4.18.20 kernel"
make olddefconfig
make -j 8
make modules_install
make install
```
##  Runtime environment variables

```
PATH_TO_MEDIA_SDK=/opt/intel
export LD_LIBRARY_PATH=${PATH_TO_MEDIA_SDK}/libva/lib:${PATH_TO_MEDIA_SDK}/mediasdk/lib64
export LIBVA_DRIVER_NAME=iHD
export LIBVA_DRIVERS_PATH=${PATH_TO_MEDIA_SDK}/media-driver/dri
```