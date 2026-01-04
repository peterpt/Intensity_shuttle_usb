# VHCapture for linux

<img width="909" height="624" alt="image" src="https://github.com/user-attachments/assets/ceffd439-36d2-4056-945e-c60f3db41cf5" />


- This project was build using google ai assistance over multiples sessions
this project uses a modiefied wrapper of libbmusb that we had to adjust for the
multiples resolutions , this driver is native and it will work with your older
i5 or amd64 cpu .




This app does not require a driver from blackmagic for linux , but it requires the reneseas usb3 chipset required from blackmagic .

## Notes to make this work :
uninstall any libbmusb-dev from your package repository
apt remove --purge libbmusb-dev 
cd libbmusb_0.7.8_optimized
apt-get install python3 python3-pip build-essential libusb-1.0-0-dev ffmpeg
make clean
make && make install && ldconfig

Python3 shuttle.py main script requirements :
pip3 install numpy pyaudio Pillow opencv-python psutil --break-system-packages

## wrapper to talk between python script and the optimized libbmusb manually installed :
shim.cpp is a c++ library to speak with between this python tool and libbmusb optimized
you already have a libshim.so library in this folder but in case it does not work on 
your system then delete it and recompile it for your system inside this git folder :
g++ -g -std=c++11 -fPIC -shared -o libshim.so shim.cpp -lbmusb $(pkg-config --cflags --libs libusb-1.0)

## Windows cross compile with wine was not yet made , but for windows it is required a dll to talk 
with intensity shuttle , and that dll it is inside this git called shim.dll
this dll was a crosscompile between our shim_win.cpp and the oficial sdk decklink api
These files are also in win directory just for checkup .

