FROM dockcross/linux-armv7

ENV DEFAULT_DOCKCROSS_IMAGE fmradion_builder
RUN apt-get update
RUN apt-get install -y git python3 python3-pip librtlsdr0 librtlsdr-dev libsndfile1 libsndfile1-dev libsox2 libsox-dev libsoxr0 libsoxr-dev libboost-all-dev
WORKDIR /nlib/
RUN wget https://dl.bintray.com/boostorg/release/1.72.0/source/boost_1_72_0.tar.gz
RUN tar -xvf boost*.gz
WORKDIR boost_1_72_0/
RUN ./bootstrap.sh
RUN ./b2 install
WORKDIR /nlib/
RUN git clone --depth 1 https://github.com/gnuradio/volk libvolk
WORKDIR libvolk
RUN pip3 install mako
WORKDIR build
RUN cmake /nlib/libvolk
RUN make -j3
RUN make install