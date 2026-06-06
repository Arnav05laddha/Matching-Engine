import csv
import random
import math

NUM_ORDERS = 1000000
START_PRICE = 50000  # $500.00
FILE_PATH = "../data/mbo_data.csv"

def generate_heavy_tailed_price(current_price, volatility=5):
    # Laplace distribution for heavy-tailed price jumps
    jump = int(random.expovariate(1.0 / volatility))
    if random.random() < 0.5:
        jump = -jump
    return max(1, min(100000, current_price + jump))

def main():
    import os
    os.makedirs(os.path.dirname(FILE_PATH), exist_ok=True)

    print("Generating 1 Million realistic Market-By-Order ticks...")
    
    current_bid = START_PRICE - 5
    current_ask = START_PRICE + 5

    active_orders = []
    
    with open(FILE_PATH, mode='w', newline='') as file:
        writer = csv.writer(file)
        writer.writerow(["order_id", "price", "qty", "side", "type"])
        
        for i in range(1, NUM_ORDERS + 1):
            # 80% chance to ADD a limit order, 20% chance to CANCEL an existing order
            if random.random() < 0.2 and active_orders:
                cancel_idx = random.randint(0, len(active_orders) - 1)
                order_id_to_cancel = active_orders.pop(cancel_idx)
                writer.writerow([order_id_to_cancel, 0, 0, 0, 1]) # type 1 is CANCEL
            else:
                side = 0 if random.random() < 0.5 else 1 # 0: BUY, 1: SELL
                qty = random.randint(1, 100)
                
                # Heavy tail price generation
                if side == 0:
                    current_bid = generate_heavy_tailed_price(current_bid)
                    price = current_bid
                else:
                    current_ask = generate_heavy_tailed_price(current_ask)
                    price = current_ask
                
                writer.writerow([i, price, qty, side, 0]) # type 0 is LIMIT
                active_orders.append(i)

    print(f"Data saved to {FILE_PATH}")

if __name__ == "__main__":
    main()
