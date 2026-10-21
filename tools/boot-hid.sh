sudo pkill -f 'nc -ukl 5140' || true
sudo pkill -f 'venus-usbnet-mac.sh' || true
fastboot boot shim/out/shim-boot-venus-v23.img
