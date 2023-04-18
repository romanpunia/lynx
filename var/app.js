const express = require('express');
const server = express();
server.use(express.static('./../bin/web'));
server.listen(8090);