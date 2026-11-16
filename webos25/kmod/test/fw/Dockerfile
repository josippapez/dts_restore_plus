FROM debian:12-slim
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends \
      git build-essential cmake liblzo2-dev libssl-dev zlib1g-dev ca-certificates \
      squashfs-tools python3 python3-pip python3-dev python3-lz4 unzip >/dev/null
RUN git clone -q --depth 1 https://github.com/openlgtv/epk2extract /opt/epk2extract \
 && cd /opt/epk2extract && CMAKE_FLAGS=-DCMAKE_BUILD_TYPE=Test ./build.sh \
 && cp keys/* build_linux/
RUN pip3 install -q --break-system-packages git+https://github.com/marin-m/vmlinux-to-elf
COPY extract.sh unlz4p.py lpunpack.py /fw/
