FROM php:8.3-cli-bookworm

RUN apt-get update && apt-get install -y \
    libxml2-dev libsqlite3-dev libcurl4-openssl-dev libssl-dev \
    libonig-dev libreadline-dev libsodium-dev libargon2-dev \
    zlib1g-dev git libbrotli-dev procps \
    && rm -rf /var/lib/apt/lists/*

RUN pecl install swoole-6.2.0 && docker-php-ext-enable swoole

COPY . /app
RUN cd /app/extension && phpize && ./configure --enable-pyroscope-php && make && make install && echo "extension=pyroscope_php.so" > /usr/local/etc/php/conf.d/pyroscope_php.ini

WORKDIR /app
EXPOSE 9501

ENV PYROSCOPE_APP_NAME=my-shop-api
ENV PYROSCOPE_ENDPOINT=http://pyroscope:4040

CMD ["php", "examples/demo_server.php"]
