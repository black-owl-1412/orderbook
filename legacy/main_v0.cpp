#include<iostream>
#include<string>
#include<cinttypes>
#include <vector> 
#include <list>
#include <memory> 
#include <map>
#include <unordered_map>
#include <numeric>


 enum class OrderType{
    GoodTillCancel,
    FillAndKill
};

enum class Side{
    Buy,
    Sell
};

using Price = std::int32_t;
using Quantity = std::uint32_t;
using OrderId = std::uint64_t;


struct LevelInfo
{
    Price price_;
    Quantity quantity_;
};

using LevelInfos = std::vector<LevelInfo>;



class OrderBookLevelInfos{
public: 
    OrderBookLevelInfos(const LevelInfos& bids, const LevelInfos& asks)
           :bids_{bids},  
            asks_{asks}
    {   }

    const LevelInfos& GetBids() {return bids_;}
    const LevelInfos& GetAsks() {return asks_;}

private: 
    LevelInfos bids_;
    LevelInfos asks_;

};


class  Order{ 
public: 
      Order(OrderType ordertype,OrderId orderid, Side side, Quantity quantity, Price price)
            : ordertype_{ordertype}, 
                orderid_{orderid},
                   side_ {side},
               initialQuantity_{quantity},
               remainingQuantity_{quantity},
                  price_{price}
                 
            
      {}                

      //public api
    
          OrderType GetOrderType() { return ordertype_; }
          OrderId GetOrderId() { return orderid_;}
          Side GetSide() {return side_;}
          Quantity GetInitialQuantity() {return initialQuantity_;}
          Quantity GetRemainingQuantity() {return remainingQuantity_;}
          Price GetPrice() { return price_; }

          void Fill(Quantity quantity){
            remainingQuantity_ -= quantity;
          }
          bool IsFilled()  { return GetRemainingQuantity() == 0;}
private:
      OrderType ordertype_;
      OrderId     orderid_;
      Side           side_;
      Quantity initialQuantity_;
      Quantity remainingQuantity_;
      Price         price_;

    };

using OrderPointer = std::shared_ptr<Order>;
using OrderPointers = std::list<OrderPointer>;

class ModifyOrder{
public:
    ModifyOrder(OrderId orderid, Side side, Quantity quantity, Price price)
    :  orderid_{orderid},
        side_ {side},
        quantity_{quantity_},
        price_{price}

    {}
    OrderId GetOrderId() { return orderid_;}
    Side GetSide() {return side_;}
    Quantity GetQuantity() {return quantity_;}
    Price GetPrice() { return price_; }
    
    OrderPointer OrderToPpointer(OrderType type){
        return std::make_shared<Order>(
            type,
            GetOrderId(),
            GetSide(),
            GetQuantity(),
            GetPrice()
        );
    }
    
    
private: 
    OrderId  orderid_;
    Side side_;
    Quantity quantity_;
    Price price_;



};


struct TradeInfo{
    OrderId  orderid_;
    Price price_;
    Quantity quantity_;
};

class Trade{ 
public:
    Trade(const TradeInfo& bidTrade, const TradeInfo& askTrade)
    :bidTrade_{bidTrade},
     askTrade_{askTrade}
     
    {}

    TradeInfo GetBidTradeInfo() {return bidTrade_;}
    TradeInfo GetAskTradeInfo() {return askTrade_;}
private:
    TradeInfo bidTrade_;
    TradeInfo askTrade_;

};



using Trades = std::vector<Trade>;










class OrderBook{
private:

    struct OrderEntry{
        OrderPointer order_{nullptr};
        OrderPointers::iterator location_;
    };
    
    std::map<Price, OrderPointers, std::greater<Price>> bids_;
    std::map<Price, OrderPointers, std::less<Price>> asks_;
    std::unordered_map<OrderId, OrderEntry> orders_;

    bool CanMatch (Side side, Price price) const {

    if(side == Side::Buy){
            if(asks_.empty())
                return false;

            const auto& [bestAsk, _] = *bids_.begin();  
            return price>=bestAsk;
    }
    else{
        if(bids_.empty())
            return false; 
        
        const auto& [bestBid, _] = *asks_.begin();
        return price<=bestBid;
    }

  }

  Trades MatchOrders(){
    Trades trades; 
    trades.reserve(orders_.size()); 
    while(true) 
    { 
        if(asks_.empty()||bids_.empty())
            break;  
        
        auto& [bidPrice, bids] = *bids_.begin();
        auto& [askPrice,  asks] = *asks_.begin();
        
        if(bidPrice < askPrice)
            break;

        while (asks.size() && bids.size())
        {
            auto& bid  = bids.front();
            auto& ask = asks.front();

            Quantity quantity = std::min(bid->GetRemainingQuantity(), ask->GetRemainingQuantity());

            bid->Fill(quantity);
            ask->Fill(quantity);

            if(bid->IsFilled())
            {
                bids.pop_front();
                orders_.erase(bid->GetOrderId());

            }
            if(ask->IsFilled())
            {
                asks.pop_front();
                orders_.erase(ask->GetOrderId());

            }

            if(bids.empty()){
                bids_.erase(bidPrice);
            }
            if(asks.empty()){
                asks_.erase(askPrice);

            }

            trades.push_back(Trade{
                TradeInfo{bid->GetOrderId(), bid->GetPrice(),quantity}, 
                TradeInfo{ask->GetOrderId(), ask->GetPrice(), quantity} 
            });


            




        }
        
        if(!bids.empty())
        {
            auto& [_, bids] = *bids_.begin();
            auto& order = bids.front();
            if(order->GetOrderType() == OrderType::FillAndKill)
                CancelOrder(order->GetOrderId());
            
        }
        if(!asks.empty())
        {
            auto& [_, asks] = *asks_.begin();
            auto& order = asks.front();
            if(order->GetOrderType() == OrderType::FillAndKill)
                CancelOrder(order->GetOrderId());
            
        }

    }
    return trades; 

  }

public:
    Trades AddOrder(OrderPointer order){
        if(orders_.count(order->GetOrderId()))
            return { };
        
        if(order->GetOrderType() == OrderType::FillAndKill && !CanMatch(order->GetSide(), order->GetPrice()))
                return {  };
        
        
        OrderPointers::iterator iterator; 

                
        if(order->GetSide() == Side::Buy){ 
            auto& orders = bids_[order->GetPrice()];
            orders.push_back(order);
            iterator = std::next(orders.begin(), orders.size()-1);


        }    
        else{ 
            auto& orders = asks_[order->GetPrice()]; 
            orders.push_back(order); 
            iterator  = std::next( orders.begin(), orders.size()-1 );
        }    

        orders_.insert({order->GetOrderId(), OrderEntry{order, iterator}});
        return MatchOrders();
    }  

    void CancelOrder(OrderId orderId){
        if(!orders_.count(orderId))
             return;
        
             

        const auto& [order,orderIterator] = orders_.at(orderId);
        
        orders_.erase(orderId);

        if(order->GetSide() == Side::Buy)
        {
            const auto price = order->GetPrice();
            auto& orders  = bids_.at(price); 
            orders.erase(orderIterator); 
            if(orders.empty())
                bids_.erase(price);
            
            
        }
        if(order->GetSide() == Side::Sell)
        {   
            const auto price = order->GetPrice(); 
            auto& orders = asks_.at(price); 
            orders.erase(orderIterator);
            if(orders.empty())
                asks_.erase(price);
                

        }
       
    }

    Trades MatchOrder(ModifyOrder order){
            if(!orders_.count(order.GetOrderId()))
                return { }; 
            
            auto& [existingOrder, _] = orders_.at(order.GetOrderId());
            CancelOrder(order.GetOrderId()); 
            return AddOrder(order.OrderToPpointer(existingOrder->GetOrderType()));
            

    }

    std::size_t Size() const {return orders_.size();}

    OrderBookLevelInfos GetOrderInfos() const { 
            LevelInfos bidInfos, askInfos;
            bidInfos.reserve(orders_.size()); 
            askInfos.reserve(orders_.size()); 

            auto CreateLevelInfos = [](const Price price, const OrderPointers& orders) { 
                return LevelInfo{price,  std::accumulate(orders.begin(),orders.end(), Quantity(0), 
                [](std::size_t runningSum, const OrderPointer& order){return runningSum + order->GetRemainingQuantity();})};
            };

            for(const auto& [price, orders] : bids_){ 
                bidInfos.push_back(CreateLevelInfos(price, orders));

            }

            for(const auto& [price, orders] : asks_){ 
                askInfos.push_back(CreateLevelInfos(price, orders));
            }

            return OrderBookLevelInfos{bidInfos,askInfos};


    }





};



int main(void){

    OrderBook orderbook;
    orderbook.AddOrder(std::make_shared<Order>(OrderType::GoodTillCancel, 1, Side::Buy, 3, 100));
    orderbook.AddOrder(std::make_shared<Order>(OrderType::GoodTillCancel, 2, Side::Sell, 7, 200));
    std::cout<<orderbook.Size()<<std::endl;
    auto levelinfos = orderbook.GetOrderInfos();

    auto asks  = levelinfos.GetAsks();

    

    orderbook.CancelOrder(1);
    std::cout<<orderbook.Size()<<std::endl;

    return 0;


}


