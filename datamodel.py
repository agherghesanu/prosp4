"""
IMC Prosperity 4 - Data Model
==============================
This is the datamodel that the competition provides.
Your algorithm imports from this file.
Based on the consistent API pattern across Prosperity 1-3.

NOTE: The actual datamodel.py is provided by IMC on the competition platform.
      This is a faithful reconstruction for local development and backtesting.
      Update if IMC changes any fields for Prosperity 4.
"""

import json
from typing import Dict, List, Any, Optional

Time = int
Symbol = str
Product = str
Position = int
UserId = str
ObservationValue = int


class Listing:
    def __init__(self, symbol: Symbol, product: Product, denomination: Product):
        self.symbol = symbol
        self.product = product
        self.denomination = denomination


class OrderDepth:
    def __init__(self):
        self.buy_orders: Dict[int, int] = {}   # price -> positive quantity
        self.sell_orders: Dict[int, int] = {}   # price -> negative quantity


class Trade:
    def __init__(self, symbol: Symbol, price: int, quantity: int,
                 buyer: UserId = "", seller: UserId = "",
                 timestamp: int = 0) -> None:
        self.symbol = symbol
        self.price: int = price
        self.quantity: int = quantity
        self.buyer = buyer
        self.seller = seller
        self.timestamp = timestamp

    def __str__(self) -> str:
        return (f"({self.symbol}, buyer={self.buyer}, seller={self.seller}, "
                f"price={self.price}, qty={self.quantity}, t={self.timestamp})")

    def __repr__(self) -> str:
        return self.__str__()


class ConversionObservation:
    """For products tradeable on foreign exchanges (e.g., Orchids, Macarons)."""
    def __init__(self, bidPrice: float, askPrice: float,
                 transportFees: float, exportTariff: float,
                 importTariff: float, sugarPrice: float,
                 sunlightIndex: float):
        self.bidPrice = bidPrice
        self.askPrice = askPrice
        self.transportFees = transportFees
        self.exportTariff = exportTariff
        self.importTariff = importTariff
        self.sugarPrice = sugarPrice
        self.sunlightIndex = sunlightIndex


class Observation:
    def __init__(self):
        self.plainValueObservations: Dict[Product, ObservationValue] = {}
        self.conversionObservations: Dict[Product, ConversionObservation] = {}


class TradingState:
    def __init__(self,
                 traderData: str,
                 timestamp: Time,
                 listings: Dict[Symbol, Listing],
                 order_depths: Dict[Symbol, OrderDepth],
                 own_trades: Dict[Symbol, List[Trade]],
                 market_trades: Dict[Symbol, List[Trade]],
                 position: Dict[Product, Position],
                 observations: Observation):
        self.traderData = traderData
        self.timestamp = timestamp
        self.listings = listings
        self.order_depths = order_depths
        self.own_trades = own_trades
        self.market_trades = market_trades
        self.position = position
        self.observations = observations

    def toJSON(self):
        return json.dumps(self, default=lambda o: o.__dict__, sort_keys=True)


class Order:
    def __init__(self, symbol: Symbol, price: int, quantity: int) -> None:
        self.symbol = symbol
        self.price = price
        self.quantity = quantity

    def __str__(self) -> str:
        return f"({self.symbol} {self.price}x{self.quantity})"

    def __repr__(self) -> str:
        return self.__str__()


class ProsperityEncoder(json.JSONEncoder):
    def default(self, o):
        return o.__dict__
