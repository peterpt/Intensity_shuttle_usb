DecklinkAPI.h from SDK 10.1.4 from blackmagic i Had to use mingw widl to convert the DeckLinkAPI.idl to DeckLinkAPI.h to crosscompile our shim_win.cpp with DecklinkAPI.h to a single dll so python script in future knows hoo to call intensity shuttle over windows environment , this was made in windows7 64bit
IMPORTANT
cross compile and testing in windows was not yet tested or done
