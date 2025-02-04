
// 增加注释

#include <binapi/websocket.hpp>
#include <binapi/types.hpp>
#include <binapi/message.hpp>
#include <binapi/fnv1a.hpp>
#include <binapi/flatjson.hpp>
#include <binapi/errors.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <boost/callable_traits.hpp>
#include <boost/algorithm/string/case_conv.hpp>

#include <boost/intrusive/set.hpp>

#include <map>
#include <set>
#include <cstring>

//#include <iostream> // TODO: comment out
// 出错时候统一使用的函数，统一封装，源自外面对于这个回调函数整体格式的定义
#define __BINAPI_CB_ON_ERROR(cb, ec) \
    cb(__FILE__ "(" BOOST_PP_STRINGIZE(__LINE__) ")", ec.value(), ec.message(), nullptr, 0);

namespace binapi {
namespace ws {

struct websockets;

/*************************************************************************************************/

struct websocket: std::enable_shared_from_this<websocket> {
    // websockets 这个是ws链接，设置友元
    friend struct websockets;
    // 上下文初始化，不使用隐式转换所有的都明确类型
    explicit websocket(boost::asio::io_context &ioctx)
        :m_ioctx{ioctx}
        ,m_ssl{boost::asio::ssl::context::sslv23_client}
        ,m_resolver{m_ioctx}
        ,m_ws{m_ioctx, m_ssl}
        ,m_buf{}
        ,m_host{}
        ,m_target{}
        ,m_stop_requested{}
    {}
    virtual ~websocket()
    {}
    // 提高代码的可读性，采用指针的形式；
    using holder_type = std::shared_ptr<websocket>;

    template<typename CB>
    void async_start(
         const std::string &host        // 主机域名
        ,const std::string &port        // 端口
        ,const std::string &target      // 修改的目标
        ,CB cb                          // 回调函数
        ,holder_type holder             // 指针输入
    ) {
        m_host = host;                  // m_host 主机变量缓存
        m_target = target;              // endpoint缓存
        // 以下内容首先是IP地址解析
        m_resolver.async_resolve(
             m_host                     // 域名
            ,port                       // 端口
            ,[this, cb=std::move(cb), holder=std::move(holder)]// 输入回调函数和自己类型的智能指针
             (boost::system::error_code ec, boost::asio::ip::tcp::resolver::results_type res) mutable {
                if ( ec ) {
                    if ( !m_stop_requested ) { __BINAPI_CB_ON_ERROR(cb, ec); } // 域名解析失败，直接调用回调函数，统一的错误处理函数
                } else {
                    async_connect(std::move(res), std::move(cb), std::move(holder)); //解析成功就开始链接，res,cb,holder
                }
            }
        );
    }

    void stop() {
        m_stop_requested = true;

        if ( m_ws.next_layer().next_layer().is_open() ) {
            // 直接关闭TCP基础链接
            boost::system::error_code ec;
            m_ws.close(boost::beast::websocket::close_code::normal, ec);
        }
    }

    void async_stop() {
        m_stop_requested = true;
        holder_type holder = shared_from_this();

        if ( m_ws.next_layer().next_layer().is_open() ) {
            // TCP 链接进行底层是否open
            m_ws.async_close(
                 boost::beast::websocket::close_code::normal    // 正常情况下的normal
                ,[holder=std::move(holder)](const boost::system::error_code &){} // 空回调函数传入 
            );
        }
    }

private:
    // async_connect 以上的链接
    template<typename CB>
    void async_connect(boost::asio::ip::tcp::resolver::results_type res, CB cb, holder_type holder) {
        // ws下一层之后的下一层native_handle
        if( !SSL_set_tlsext_host_name(m_ws.next_layer().native_handle() ,m_host.c_str())) {
            auto error_code = boost::beast::error_code(
                 static_cast<int>(::ERR_get_error())     // 全局作用域函数
                ,boost::asio::error::get_ssl_category()  // 获取 get_ssl_category分类
            );

            __BINAPI_CB_ON_ERROR(cb, error_code);    // 直接采用回调函数

            return;
        }

        boost::asio::async_connect(
             m_ws.next_layer().next_layer()            // 底层TCP
            ,res.begin()
            ,res.end()
            ,[this, cb=std::move(cb), holder=std::move(holder)]
             (boost::system::error_code ec, boost::asio::ip::tcp::resolver::iterator) mutable {
                  // 直接输入，另外一个只作为形参，为了保留接口一致性
                if ( ec ) {
                    if ( !m_stop_requested ) { __BINAPI_CB_ON_ERROR(cb, ec); }
                } else {
                    on_connected(std::move(cb), std::move(holder));
                }
            }
        );
    }
    // 此处采用控制回调函数 
    template<typename CB>
    void on_connected(CB cb, holder_type holder) {
        // 网络连接处理分类，除了业务类之后还有ping  pong  close
        m_ws.control_callback(
            [this]
            (boost::beast::websocket::frame_type kind, boost::beast::string_view payload) mutable {
                (void)kind; (void) payload;
                //std::cout << "control_callback(" << this << "): kind=" << static_cast<int>(kind) << ", payload=" << payload.data() << std::endl;
                m_ws.async_pong(
                     boost::beast::websocket::ping_data{}
                    ,[](boost::beast::error_code ec)
                     { (void)ec; /*std::cout << "control_callback_cb(" << this << "): ec=" << ec << std::endl;*/ }
                );
            }
        );
        // SSL层进行握手
        m_ws.next_layer().async_handshake(
             boost::asio::ssl::stream_base::client
            ,[this, cb=std::move(cb), holder=std::move(holder)]
             (boost::system::error_code ec) mutable {
                if ( ec ) {
                    if ( !m_stop_requested ) { __BINAPI_CB_ON_ERROR(cb, ec); }
                } else {
                    on_async_ssl_handshake(std::move(cb), std::move(holder));
                }
            }
        );
    }
    // start_read cb 肯定是要传导下来的，这么多层嵌套一层一层下来，堆栈已经崩溃了
    template<typename CB>
    void on_async_ssl_handshake(CB cb, holder_type holder) {
        m_ws.async_handshake(
             m_host
            ,m_target
            ,[this, cb=std::move(cb), holder=std::move(holder)]
             (boost::system::error_code ec) mutable
             { start_read(ec, std::move(cb), std::move(holder)); }
        );
    }
    // start_read 包装了一层的read，增加鲁棒性描述，每一步都检查上一步是否出问题，这样上面出问题，进入下一层函数的时候能够及时停止
    template<typename CB>
    void start_read(boost::system::error_code ec, CB cb, holder_type holder) {
        if ( ec ) {
            if ( !m_stop_requested ) {
                __BINAPI_CB_ON_ERROR(cb, ec);
            }

            stop();

            return;
        }

        m_ws.async_read(
             m_buf
            ,[this, cb=std::move(cb), holder=std::move(holder)]
             (boost::system::error_code ec, std::size_t rd) mutable
             { on_read(ec, rd, std::move(cb), std::move(holder)); }
        );
    }
// read的细节
// 
    template<typename CB>
    void on_read(boost::system::error_code ec, std::size_t rd, CB cb, holder_type holder) {
        if ( ec ) {
            if ( !m_stop_requested ) {
                __BINAPI_CB_ON_ERROR(cb, ec);
            }

            stop();

            return;
        }
        // 新变量作为一个缓存而存在
        // strbuf是作为一个字符串存在
        auto size = m_buf.size();
        assert(size == rd);

        std::string strbuf;
        strbuf.reserve(size);

        for ( const auto &it: m_buf.data() ) {
            strbuf.append(static_cast<const char *>(it.data()), it.size());
        }
        // 释放已经处理过的数据，单连接的缓存确实可以
        m_buf.consume(m_buf.size());
        
        // ok是什么样的cb回调函数
        bool ok = cb(nullptr, 0, std::string{}, strbuf.data(), strbuf.size());
        if ( !ok ) {
            stop();
        } else {
            // 两个一直循环读取循环处理cb函数，这里的标记
            start_read(boost::system::error_code{}, std::move(cb), std::move(holder));
        }
    }

    boost::asio::io_context &m_ioctx;            // 异步io上下文环境
    boost::asio::ssl::context m_ssl;             // ssl异步上下文环境
    boost::asio::ip::tcp::resolver m_resolver;   // 域名dns解析
    boost::beast::websocket::stream<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>> m_ws; // 管理安全的ws流，不同layer之间切换ws,ssl,tcp
    boost::beast::multi_buffer m_buf;  // 缓冲区，动态分配，允许自动分配内存，进行大量的读写操作
    std::string m_host;                // host主机
    std::string m_target;              // 目标，也就是资源的端点
    bool m_stop_requested;             // 停止访问
    boost::intrusive::set_member_hook<> m_intrusive_set_hook;// 一种数据类型，用于连接统一类型的数据
};

// 针对整个websocket结构体函数的一种类型
struct websocket_id_getter {
    using type = const void *;
    type operator()(const websocket &sock) const { return std::addressof(sock); }// 唯一的id对象地址
};

/*************************************************************************************************/
/*************************************************************************************************/
/*************************************************************************************************/
// 大量的websocket连接管理函数
struct websockets::impl {
    impl(
         boost::asio::io_context &ioctx   // 祖传异步io上下文环境
        ,std::string host                 // 祖传网络访问必定存在地址
        ,std::string port                 // 祖传端口
        ,on_message_received_cb msg_cb    // 回调函数管理，因为异步IO也自动管理回调函数
        ,on_network_stat_cb stat_cb       // 祖传回调函数，网络状态的回调函数，其它也存在类似的思路，根据不同状态进行处理的log等等
        ,std::size_t stat_interval        // 状态回调间隔，这个也是没有上限的，需要合理设置高频情况下，采用极限值进行操作
    )
        :m_ioctx{ioctx}
        ,m_host{std::move(host)}
        ,m_port{std::move(port)}
        ,m_on_message{std::move(msg_cb)}
        ,m_on_stat{std::move(stat_cb)}
        ,m_stat_interval{stat_interval}
        ,m_set{}
    {}
    ~impl() {
        unsubscribe_all();
    }
    // 访问的endpoint，字符串处理小工具
    static std::string make_channel_name(const char *pair, const char *channel) {
        std::string res{"/ws/"};
        if ( pair ) {
            res += pair;
            if ( *pair != '!' ) {
                boost::algorithm::to_lower(res);
            }

            res += '@';
        }

        res += channel;

        return res;
    }
    // 单独频道开始死啦死啦滴获取数据
    template<typename F>
    websockets::handle start_channel(const char *pair, const char *channel, F cb) {
        using args_tuple = typename boost::callable_traits::args<F>::type; // 获取回调函数的列表
        using message_type = typename std::tuple_element<3, args_tuple>::type;// 获取回调函数的第四个类型，也就是自己制造的结构体
        // 智能指针的删除器，如何生、如何灭，这是一门学问
        static const auto deleter = [this](websocket *ws) {
            auto it = m_set.find(ws);
            if ( it != m_set.end() ) {
                m_set.erase(it);
            }

            delete ws;
        };
        // 智能指针绑定删除器
        std::shared_ptr<websocket> ws{new websocket(m_ioctx), deleter};
        std::string schannel = make_channel_name(pair, channel); //channel字符串直接进行组合比如 /api/v3/ticker/price

        // 通用回调函数，实际上是从外部输入的回调函数
        auto wscb = [this, schannel, cb=std::move(cb)]    // 回调函数的引用列表
            (const char *fl, int ec, std::string errmsg, const char *ptr, std::size_t size) -> bool
        {
            if ( ec ) {
                try {
                    cb(fl, ec, std::move(errmsg), message_type{});
                } catch (const std::exception &ex) {
                    std::fprintf(stderr, "%s: %s\n", __MAKE_FILELINE, ex.what());
                    std::fflush(stderr);
                }

                return false;
                // 错误就提前终止函数
            }

            const flatjson::fjson json{ptr, size};
            if ( json.is_object() && binapi::rest::is_api_error(json) ) {
                // 错误码就直接进行查询并进行返回,这个时候处理的错误实际上已经是网路成功之后的错误，因此错误就是直接对接api列表
                auto error = binapi::rest::construct_error(json); // 错误码用的居然是rest，api错误码本身应该更加通用，后续重新尝试移动位置
                auto ecode = error.first;                         // error，错误码
                auto emsg  = std::move(error.second);             // 错误消息

                try {
                    message_type message{};        //来自回调函数的空结构体
                    return cb(__MAKE_FILELINE, ecode, std::move(emsg), std::move(message)); // 调用回调函数，回调函数可以进行处理，通用回调函数直接注入引擎
                } catch (const std::exception &ex) {
                    std::fprintf(stderr, "%s: %s\n", __MAKE_FILELINE, ex.what());
                    std::fflush(stderr);
                }
            }

            try {
                if ( m_on_message ) { m_on_message(schannel.c_str(), ptr, size); } // message进行处理有消息就进行处理
            } catch (const std::exception &ex) {
                std::fprintf(stderr, "%s: %s\n", __MAKE_FILELINE, ex.what());
                std::fflush(stderr);
            }

            try {
                message_type message = message_type::construct(json);             // 完成从消息到结构体的赋值
                return cb(nullptr, 0, std::string{}, std::move(message));         // 得到结构体后调用回调函数
            } catch (const std::exception &ex) {
                std::fprintf(stderr, "%s: %s\n", __MAKE_FILELINE, ex.what());
                std::fflush(stderr);
            }

            return false;
        };

        // 这个ws的地址获取，先进行启动，毕竟属于start channel
        auto *ws_ptr = ws.get();
        ws_ptr->async_start(
             m_host
            ,m_port
            ,schannel
            ,std::move(wscb)
            ,std::move(ws)
        );

        m_set.insert(*ws_ptr);  // 将连接直接add进入总体的多个websocket链接中

        return ws_ptr;
    }

    // 停止stop_channel_impl进行 模板，f函数对ws进行处理
    template<typename F>
    void stop_channel_impl(handle h, F f) {
        auto it = m_set.find(h);
        if ( it == m_set.end() ) { return; }

        auto *ws = static_cast<websocket *>(&(*it));
        f(ws);

        m_set.erase(it);
    }

    void stop_channel(handle h) {
        // 调用ws的stop
        return stop_channel_impl(h, [](auto sp){ sp->stop(); });
    }
    void async_stop_channel(handle h) {
        // 调用sp的async_stop
        return stop_channel_impl(h, [](auto sp){ sp->async_stop(); });
    }

    template<typename F>
    void unsubscribe_all_impl(F f) {
        // F f 这个m_set进行所有的链接都进行调用
        for ( auto it = m_set.begin(); it != m_set.end(); ) {
            auto *ws = static_cast<websocket *>(&(*it));
            f(ws);

            it = m_set.erase(it);
        }
    }
    void unsubscribe_all() {
        // 回调函数传入
        return unsubscribe_all_impl([](auto sp){ sp->stop(); });
    }
    void async_unsubscribe_all() {
        // 取消所有的回调函数进行传入
        return unsubscribe_all_impl([](auto sp){ sp->async_stop(); });
    }

    boost::asio::io_context &m_ioctx;        // m_ioctx,上下文环境
    std::string m_host;                      // host
    std::string m_port;                      // port
    on_message_received_cb m_on_message;     // 消息回调函数
    on_network_stat_cb m_on_stat;            // 网络状态
    std::size_t m_stat_interval;             // 状态更新
// 连接集合管理函数
    boost::intrusive::set<
         websocket
        ,boost::intrusive::key_of_value<websocket_id_getter>
        ,boost::intrusive::member_hook<
             websocket
            ,boost::intrusive::set_member_hook<>
            ,&websocket::m_intrusive_set_hook
        >
    > m_set;
};

/*************************************************************************************************/

websockets::websockets(
     boost::asio::io_context &ioctx
    ,std::string host
    ,std::string port
    ,on_message_received_cb msg_cb
    ,on_network_stat_cb stat_cb
    ,std::size_t stat_interval
)
    :pimpl{std::make_unique<impl>(
         ioctx
        ,std::move(host)
        ,std::move(port)
        ,std::move(msg_cb)
        ,std::move(stat_cb)
        ,stat_interval
    )}
{}

websockets::~websockets()
{}

/*************************************************************************************************/

websockets::handle websockets::part_depth(const char *pair, e_levels level, e_freq freq, on_part_depths_received_cb cb) {
    std::string ch = "depth";
    ch += std::to_string(static_cast<std::size_t>(level));
    ch += "@";
    ch += std::to_string(static_cast<std::size_t>(freq)) + "ms";
    return pimpl->start_channel(pair, ch.c_str(), std::move(cb));
}

/*************************************************************************************************/

websockets::handle websockets::diff_depth(const char *pair, e_freq freq, on_diff_depths_received_cb cb) {
    std::string ch = "depth@" + std::to_string(static_cast<std::size_t>(freq)) + "ms";
    return pimpl->start_channel(pair, ch.c_str(), std::move(cb));
}

/*************************************************************************************************/

websockets::handle websockets::klines(const char *pair, const char *interval, on_kline_received_cb cb) {
    static const auto switch_ = [](const char *interval) -> const char * {
        const auto hash = fnv1a(interval);
        switch ( hash ) {
            // secs
            case fnv1a("1s"): return "kline_1s";
            // mins
            case fnv1a("1m"): return "kline_1m";
            case fnv1a("3m"): return "kline_3m";
            case fnv1a("5m"): return "kline_5m";
            case fnv1a("15m"): return "kline_15m";
            case fnv1a("30m"): return "kline_30m";
            // hours
            case fnv1a("1h"): return "kline_1h";
            case fnv1a("2h"): return "kline_2h";
            case fnv1a("4h"): return "kline_4h";
            case fnv1a("6h"): return "kline_6h";
            case fnv1a("8h"): return "kline_8h";
            case fnv1a("12h"): return "kline_12h";
            // days
            case fnv1a("1d"): return "kline_1d";
            case fnv1a("3d"): return "kline_3d";
            // other
            case fnv1a("1w"): return "kline_1w";
            case fnv1a("1M"): return "kline_1M";
            //
            default: return nullptr;
        }
    };

    const char *p = switch_(interval);
    assert(p != nullptr);

    return pimpl->start_channel(pair, p, std::move(cb));
}

/*************************************************************************************************/

websockets::handle websockets::trade(const char *pair, on_trade_received_cb cb)
{ return pimpl->start_channel(pair, "trade", std::move(cb)); }

/*************************************************************************************************/

websockets::handle websockets::agg_trade(const char *pair, on_agg_trade_received_cb cb)
{ return pimpl->start_channel(pair, "aggTrade", std::move(cb)); }

/*************************************************************************************************/

websockets::handle websockets::mini_ticker(const char *pair, on_mini_ticker_received_cb cb)
{ return pimpl->start_channel(pair, "miniTicker", std::move(cb)); }

websockets::handle websockets::mini_tickers(on_mini_tickers_received_cb cb)
{ return pimpl->start_channel("!miniTicker", "arr", std::move(cb)); }

/*************************************************************************************************/

websockets::handle websockets::market(const char *pair, on_market_received_cb cb)
{ return pimpl->start_channel(pair, "ticker", std::move(cb)); }

websockets::handle websockets::markets(on_markets_received_cb cb)
{ return pimpl->start_channel("!ticker", "arr", std::move(cb)); }

/*************************************************************************************************/

websockets::handle websockets::book(const char *pair, on_book_received_cb cb)
{ return pimpl->start_channel(pair, "bookTicker", std::move(cb)); }

/*************************************************************************************************/

websockets::handle websockets::userdata(
     const char *lkey
    ,on_account_update_cb account_update
    ,on_balance_update_cb balance_update
    ,on_order_update_cb order_update)
{
    auto cb = [acb=std::move(account_update), bcb=std::move(balance_update), ocb=std::move(order_update)]
        (const char *fl, int ec, std::string errmsg, userdata::userdata_stream_t msg)
    {
        if ( ec ) {
            acb(fl, ec, errmsg, userdata::account_update_t{});
            bcb(fl, ec, errmsg, userdata::balance_update_t{});
            ocb(fl, ec, std::move(errmsg), userdata::order_update_t{});

            return false;
        }

        const flatjson::fjson json{msg.data.c_str(), msg.data.length()};
        assert(json.contains("e"));
        const auto e = json.at("e");
        const auto es = e.to_sstring();
        const auto ehash = fnv1a(es.data(), es.size());
        switch ( ehash ) {
            case fnv1a("outboundAccountPosition"): {
                userdata::account_update_t res = userdata::account_update_t::construct(json);
                return acb(fl, ec, std::move(errmsg), std::move(res));
            }
            case fnv1a("balanceUpdate"): {
                userdata::balance_update_t res = userdata::balance_update_t::construct(json);
                return bcb(fl, ec, std::move(errmsg), std::move(res));
            }
            case fnv1a("executionReport"): {
                userdata::order_update_t res = userdata::order_update_t::construct(json);
                return ocb(fl, ec, std::move(errmsg), std::move(res));
            }
            case fnv1a("listStatus"): {
                assert(!"not implemented");
                return false;
            }
            default: {
                assert(!"unreachable");
                return false;
            }
        }

        return false;
    };

    return pimpl->start_channel(nullptr, lkey, std::move(cb));
}

/*************************************************************************************************/

void websockets::unsubscribe(const handle &h) { return pimpl->stop_channel(h); }
void websockets::async_unsubscribe(const handle &h) { return pimpl->async_stop_channel(h); }

void websockets::unsubscribe_all() { return pimpl->unsubscribe_all(); }
void websockets::async_unsubscribe_all() { return pimpl->async_unsubscribe_all(); }

/*************************************************************************************************/
/*************************************************************************************************/
/*************************************************************************************************/

} // ns ws
} // ns binapi
