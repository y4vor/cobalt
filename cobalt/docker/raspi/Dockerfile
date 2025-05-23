ARG BASE_IMAGE_SHA=sha256:be15f84c44c4ee68b4f187128f0278df1b0f424c04fb5f08b098967d5b896388

FROM marketplace.gcr.io/google/debian12@${BASE_IMAGE_SHA}

# Install any necessary dependencies
# NOTE: From libxcomposite down, these are minimal requirements to run a
# V8 snapshot binary, on X11/GTK builds
#     gcc-multilib  for cross-builds of v8/32bit arm
RUN apt-get update && apt-get install -y \
    curl \
    python3-dev \
    git \
    xz-utils \
    pkgconf \
    libglib2.0-0 \
    libnss3 \
    gperf \
    libdbus-1-3 \
    libxcomposite1 \
    libxdamage1 \
    libxext6 \
    libxfixes3 \
    libxrandr2 \
    libxtst6 \
    libgbm1 \
    libasound2 \
    libxkbcommon0 \
    libpango-1.0-0 \
    gcc-multilib \
    bzip2 \
    jq \
    && rm -rf /var/lib/apt/lists/*

ARG GCLOUD=google-cloud-cli-linux-x86_64.tar.gz
RUN cd /tmp \
    && curl -L -O "https://dl.google.com/dl/cloudsdk/channels/rapid/downloads/${GCLOUD}" \
    && tar xf "${GCLOUD}" -C /usr/local --strip-components=1 google-cloud-sdk/bin google-cloud-sdk/lib \
    && rm "${GCLOUD}" \
    && gcloud --version

ARG raspi_home=/root/raspi-home
ARG raspi_tools=cobalt_raspi_tools.tar.bz2

# Required by the gyp build system.
ENV RASPI_HOME=${raspi_home}

# libxml2 and binutils* needed for evergreen
RUN apt update -qqy \
    && apt install -qqy --no-install-recommends \
        g++-multilib \
        bzip2 \
        libxml2 \
        binutils-aarch64-linux-gnu \
        binutils-arm-linux-gnueabi \
        libglib2.0-dev \
    && rm -rf /var/lib/apt/lists/*

# Get the combined toolchains package.
RUN cd /tmp \
    && curl --silent -O -J \
    "https://storage.googleapis.com/cobalt-static-storage-public/${raspi_tools}" \
    && mkdir -p ${raspi_home} \
    && cd ${raspi_home} \
    && tar xjvf /tmp/${raspi_tools} --no-same-owner \
    && rm /tmp/${raspi_tools}
