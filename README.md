## RAOP player and library (AirPlay)

Modified version of cliraop/raop-play for Music Assistant.

Based on libraop by philippe44 (all rights reserved). See upstream repo for more info.

## Building

```sh
apt-get update
apt-get install -y build-essential cmake  libssl-dev
git clone https://github.com/music-assistant/libraop.git
cd libraop
git submodule update --init

# Build project
./build.sh

```
