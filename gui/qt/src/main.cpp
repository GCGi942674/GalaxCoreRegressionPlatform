#include "MainWindow.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QHostAddress>
#include <cstdio>
#include <stdexcept>

int main(int argc,char** argv) {
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication app(argc,argv);
    app.setApplicationName("GCRPGui");
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("GalaxCore Regression Platform 控制台"));
    parser.addHelpOption();
    parser.addOption({"ip",QStringLiteral("Server IP 地址（必填）"),"address"});
    parser.addOption({"port",QStringLiteral("Server TCP 端口（必填：1–65535）"),"port"});
    parser.process(app);
    try {
        if(!parser.positionalArguments().isEmpty()||!parser.isSet("ip")||!parser.isSet("port"))
            throw std::runtime_error("required: --ip ADDRESS --port PORT");
        GuiSettings settings;settings.host=parser.value("ip");QHostAddress address;
        if(!address.setAddress(settings.host)||address.isNull()||
            address==QHostAddress(QHostAddress::AnyIPv4)||address==QHostAddress(QHostAddress::AnyIPv6))
            throw std::runtime_error("--ip must be a non-wildcard IPv4 or IPv6 address");
        const auto text=parser.value("port");bool ok=false;const auto port=text.toUInt(&ok);
        for(const auto ch:text)if(ch<'0'||ch>'9')ok=false;
        if(!ok||port<1||port>65535)throw std::runtime_error("--port must be 1..65535");
        settings.port=quint16(port);
        MainWindow window(settings);window.show();return app.exec();
    }catch(const std::exception& e){
        std::fprintf(stderr,"GUI: %s\nUsage: gcrp_gui --ip ADDRESS --port PORT\n",e.what());return 64;
    }
}
