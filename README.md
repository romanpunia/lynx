## About
Lynx is a high performance async HTTP web server.

## Dependencies
* [Vitex (submodule)](https://github.com/romanpunia/vitex)

## Building
To build this project you have to clone Vitex, also you need to make sure that it can be build on your machine. CMake's **VI_DIRECTORY** is a path to Vitex source folder.

## Benchmark
There is a **/bin/web/favicon.ico** which is Google Chrome logo (32x32). To measure raw performance we will request this resource. Measurement will be done relative to Node.js with Express.js package.

Lynx must be run with following setup:
- Build in release mode
- Disable debug messages
- Disable request logging (\<log-requests\> = FALSE)
- Disable terminal (\<show-terminal\> = FALSE)
- Lynx will be running at http://127.0.0.1:8080

Node.js must be run with following setup (located at /var):
- Install (npm i)
- Node.js will be running at http://127.0.0.1:8090 (node app.js)

To benchmark you must install \<autocannon\> from npm (npm i autocannon -g). After installing you may proceed:
- Test Lynx: autocannon http://localhost:8080/favicon.ico -d 10 -c 30 -w 3
- Test Node: autocannon http://localhost:8090/favicon.ico -d 10 -c 30 -w 3

## License
Lynx is licensed under the MIT license