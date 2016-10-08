#include "application.h"
#include "OICServer.h"
#include "QDebug"
#include "QVariant"
#include <poll.h>


extern uint64_t get_current_ms();

Application::Application(int &argc, char *argv[]) : QCoreApplication(argc, argv)
{
    server = new OICServer("Orange PI Kuchnia", "0000B960-0000-46F7-BEC0-9E6CBD61ADC2", [&](COAPPacket* packet){
        this->send_packet(packet);
    });

    cbor* floorInitial = new cbor(CBOR_TYPE_MAP);
    floorInitial->append("rt", "oic.r.light.dimming");
    floorInitial->append("dimmingSetting", 5);
    floorInitial->append("range", "0,255");

    cbor* tableInitial = new cbor(CBOR_TYPE_MAP);
    tableInitial->append("rt", "oic.r.light.dimming");
    tableInitial->append("dimmingSetting", 5);
    tableInitial->append("range", "0,255");

    cbor* kuchniaInitial = new cbor(CBOR_TYPE_MAP);
    kuchniaInitial->append("rt", "oic.r.light.dimming");
    kuchniaInitial->append("dimmingSetting", 5);
    kuchniaInitial->append("range", "0,255");

    OICResource* floor = new OICResource("/lampa/floor", "oic.r.light.dimming","oic.if.rw", [=](cbor data){
        floorInitial->toMap()->insert("dimmingSetting", data.getMapValue("dimmingSetting"));
        int val = data.getMapValue("dimmingSetting").toInt();
        qDebug() << "Front updated" << val;
        setOutput(1, val);
    }, floorInitial);


    OICResource* table = new OICResource("/lampa/table", "oic.r.light.dimming","oic.if.rw", [=](cbor data){
        tableInitial->toMap()->insert("dimmingSetting", data.getMapValue("dimmingSetting"));
        int val = data.getMapValue("dimmingSetting").toInt();
        qDebug() << "Table updated" << val;
        setOutput(2, val);
    }, tableInitial);

    OICResource* kuchnia = new OICResource("/lampa/kuchnia", "oic.r.light.dimming","oic.if.rw", [=](cbor data){
        kuchniaInitial->toMap()->insert("dimmingSetting", data.getMapValue("dimmingSetting"));
        int val = data.getMapValue("dimmingSetting").toInt();
        qDebug() << "Kucyhnia updated" << val;
        setOutput(3, val);
    }, kuchniaInitial);

    server->addResource(floor);
    server->addResource(table);
    server->addResource(kuchnia);

    server->start();

    m_running = true;
    pthread_create(&m_thread, NULL, &Application::run, this);
    pthread_create(&m_discoveryThread, NULL, &Application::runDiscovery, this);


    QString port;

    if (port.isEmpty())
    {
        QList<QSerialPortInfo> infos = QSerialPortInfo::availablePorts();
        QSerialPortInfo port;
        foreach (QSerialPortInfo info, infos)
        {
            qDebug() << info.portName();
            if (info.portName().contains("USB"))
            {
                port = info;
                break;
            }
        }

        m_serial = new QSerialPort(port, this);
    }
    else
    {
        m_serial = new QSerialPort(port, this);
    }

    bool res = m_serial->open(QIODevice::ReadWrite);
    qDebug() << "Serial port opened" << res;
    m_serial->setBaudRate(QSerialPort::Baud115200);
    m_serial->setDataBits(QSerialPort::Data8);
    m_serial->setParity(QSerialPort::NoParity);
    m_serial->setStopBits(QSerialPort::OneStop);
    m_serial->setFlowControl(QSerialPort::NoFlowControl);
}

bool Application::isRunning(){
    return m_running;
}


void Application::setOutput(quint8 out, quint16 value){
    QString data("%1 %2\n");
    if (m_serial->isOpen()){
        m_serial->write(data.arg(out).arg(value).toLatin1());
        m_serial->waitForBytesWritten(1000);
    }


}

Application::~Application(){
    m_running = false;
    pthread_join(m_thread, 0);
    pthread_join(m_discoveryThread, 0);
    delete server;
}

String Application::convertAddress(sockaddr_in a){
    char addr[30];
    sprintf(addr, "%d.%d.%d.%d %d",
            (uint8_t) (a.sin_addr.s_addr),
            (uint8_t) (a.sin_addr.s_addr >> 8),
            (uint8_t) (a.sin_addr.s_addr >> 16 ),
            (uint8_t) (a.sin_addr.s_addr >> 24),
            htons(a.sin_port));

    return addr;
}

void* Application::run(void* param){
    Application* a = (Application*) param;
    OICServer* oic_server = a->getServer();

    const int on = 1;
    int fd = socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    a->setSocketFd(fd);

    struct sockaddr_in serv,client;

    serv.sin_family = AF_INET;
    serv.sin_port = 0;
    serv.sin_addr.s_addr = htonl(INADDR_ANY);

    uint8_t buffer[1024];
    socklen_t l = sizeof(client);
    if(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0)
    {
        qDebug("Unable to set reuse");
        return 0;
    }
    if( bind(fd, (struct sockaddr*)&serv, sizeof(serv) ) == -1)
    {
        qDebug("Unable to bind");
        return 0;
    }
    struct pollfd pfd;
    int res;

    size_t rc;
    int64_t lastTick = get_current_ms();

    pfd.fd = fd;
    pfd.events = POLLIN;
    while(1){
        rc = poll(&pfd, 1, 20); // 1000 ms timeout
        if (rc >0){
            rc = recvfrom(fd,buffer,sizeof(buffer),0,(struct sockaddr *)&client,&l);
            COAPPacket* p = COAPPacket::parse(buffer, rc, a->convertAddress(client).c_str());

            oic_server->handleMessage(p);
            delete p;
        }
        oic_server->sendQueuedPackets();
        if ((get_current_ms() - lastTick) > 1000){
            lastTick = get_current_ms();
            oic_server->checkPackets();
        }
    }

    return 0;
}


void* Application::runDiscovery(void* param){
    Application* a = (Application*) param;
    OICServer* oic_server = a->getServer();

    const int on = 1;

    int fd = socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    struct sockaddr_in serv,client;
    struct ip_mreq mreq;

    serv.sin_family = AF_INET;
    serv.sin_port = htons(5683);
    serv.sin_addr.s_addr = htonl(INADDR_ANY);

    mreq.imr_multiaddr.s_addr=inet_addr("224.0.1.187");
    mreq.imr_interface.s_addr=htonl(INADDR_ANY);
    if (setsockopt(fd,IPPROTO_IP,IP_ADD_MEMBERSHIP,&mreq,sizeof(mreq)) < 0) {
        return 0;
    }

    uint8_t buffer[1024];
    socklen_t l = sizeof(client);
    if(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0)
    {
        qDebug() << "Unable to set reuse";
        return 0;
    }
    if( bind(fd , (struct sockaddr*)&serv, sizeof(serv) ) == -1)
    {
        qDebug() << "Unable to bind";
        return 0;
    }
    struct pollfd pfd;
    int res;

    pfd.fd = fd;
    pfd.events = POLLIN;
    size_t rc;
    int64_t lastTick = get_current_ms();

    while(a->isRunning()){
        rc = poll(&pfd, 1, 200); // 1000 ms timeout
        if(rc > 0)
        {
            rc= recvfrom(fd,buffer,sizeof(buffer),0,(struct sockaddr *)&client,&l);
            COAPPacket* p = COAPPacket::parse(buffer, rc, a->convertAddress(client));
            oic_server->handleMessage(p);
            delete p;
        }
    }
    return 0;
}
void Application::send_packet(COAPPacket* packet){
    String destination = packet->getAddress();
    size_t pos = destination.find(" ");
    String ip = destination.substr(0, pos);
    uint16_t port = atoi(destination.substr(pos).c_str());

    struct sockaddr_in client;

    client.sin_family = AF_INET;
    client.sin_port = htons(port);
    client.sin_addr.s_addr = inet_addr(ip.c_str());

    qDebug() << "Send packet mid" << packet->getMessageId() << "dest=" << destination.c_str();
    send_packet(client, packet);
}
void Application::send_packet(sockaddr_in destination, COAPPacket* packet){
    uint8_t buffer[1024];
    size_t response_len;
    socklen_t l = sizeof(destination);
    packet->build(buffer, &response_len);
    sendto(m_socketFd, buffer, response_len, 0, (struct sockaddr*)&destination, l);
}

void Application::notifyObservers(QString name, QVariant v){
    if (server !=0){
        qDebug() << "notiftyObservers" << name << v;
        cbor value(CBOR_TYPE_MAP);

        if (name.contains("ambient")){
            value.append("rt", "oic.r.colour.rgb");
            value.append("dimmingSetting", v.toString().toLatin1().data());
        }else{
            value.append("rt", "oic.r.light.dimming");
            value.append("dimmingSetting", v.toInt());
            value.append("range", "0,255");
        }

        List<uint8_t> data;
        value.dump(&data);
        server->notify(name.toLatin1().data(), &data);
    }
}

