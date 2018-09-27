
# nginx邮件代理

## 通过配置直接与上游服务器进行认证，跳过本地认证
![照片0](https://github.com/gchs2012/nginx-mail-proxy/blob/master/image/照片0.png)

### 1、下游客户端与nginx普通连接，nginx与上游服务器普通连接
![照片1](https://github.com/gchs2012/nginx-mail-proxy/blob/master/image/照片1.png)

### 2、下游客户端与nginx SSL连接，nginx与上游服务器普通连接
![照片2](https://github.com/gchs2012/nginx-mail-proxy/blob/master/image/照片2.png)

### 3、下游客户端与nginx普通连接，nginx与上游服务器SSL连接
![照片3](https://github.com/gchs2012/nginx-mail-proxy/blob/master/image/照片3.png)

### 4、下游客户端与nginx SSL连接，nginx与上游服务器SSL连接
![照片4](https://github.com/gchs2012/nginx-mail-proxy/blob/master/image/照片4.png)

***【注】<br>***
***1、后续将继续支持STARTTLS方式<br>***
***2、后续将继续支持邮件内容如何获取<br>***
