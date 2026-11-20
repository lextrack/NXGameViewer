FROM ubuntu:22.04

ARG QT_VERSION=6.12.0
ARG AQT_VERSION=3.3.0
ARG CMAKE_VERSION=3.31.6
ARG LINUXDEPLOY_VERSION=1-alpha-20251107-1
ARG LINUXDEPLOY_QT_VERSION=1-alpha-20250213-1

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential ninja-build pkg-config git ca-certificates curl file \
        python3 python3-pip \
        libopus-dev libzstd-dev zlib1g-dev libssl-dev \
        libgl1-mesa-dev libegl1-mesa-dev libxkbcommon-dev libxkbcommon-x11-0 libfontconfig1-dev libfreetype6-dev \
        libdbus-1-dev libglib2.0-0 libpulse-dev libasound2-dev \
        libxcb1 libxcb-cursor0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 libxcb-randr0 libxcb-render-util0 \
        libxcb-shape0 libxcb-xfixes0 libxcb-xinerama0 libxcb-xkb1 libx11-xcb1 \
        libwayland-client0 libwayland-cursor0 libwayland-egl1 libxrandr2 libgtk-3-0 libcups2 \
    && rm -rf /var/lib/apt/lists/*

RUN pip3 install --no-cache-dir "aqtinstall==${AQT_VERSION}" "cmake==${CMAKE_VERSION}" \
    && aqt install-qt linux desktop "${QT_VERSION}" linux_gcc_64 -m qtmultimedia -O /opt/qt \
    && rm -rf /root/.cache

RUN curl -fsSL -o /usr/local/bin/linuxdeploy \
        "https://github.com/linuxdeploy/linuxdeploy/releases/download/${LINUXDEPLOY_VERSION}/linuxdeploy-x86_64.AppImage" \
    && curl -fsSL -o /usr/local/bin/linuxdeploy-plugin-qt \
        "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/${LINUXDEPLOY_QT_VERSION}/linuxdeploy-plugin-qt-x86_64.AppImage" \
    && chmod +x /usr/local/bin/linuxdeploy /usr/local/bin/linuxdeploy-plugin-qt

ENV QT_ROOT=/opt/qt/${QT_VERSION}/gcc_64
ENV PATH=${QT_ROOT}/bin:${PATH}
ENV APPIMAGE_EXTRACT_AND_RUN=1

WORKDIR /src
CMD ["bash", "packaging/linux/build-appimage.sh"]
