{% include 'header.sql.jinja' %}

select sum (ss_quantity)
 from {{store_sales}}, {{store}}, {{customer_demographics}}, {{customer_address}}, {{date_dim}}
 where s_store_sk = ss_store_sk
 and  ss_sold_date_sk = d_date_sk and d_year = 2000
 and
 (
  (
   cd_demo_sk = ss_cdemo_sk
   and
   cd_marital_status = 'M'
   and
   cd_education_status = '4 yr Degree'
   and
   ss_sales_price between 100.00::numeric and 150.00::numeric
   )
 or
  (
  cd_demo_sk = ss_cdemo_sk
   and
   cd_marital_status = 'D'
   and
   cd_education_status = '2 yr Degree'
   and
   ss_sales_price between 50.00::numeric and 100.00::numeric
  )
 or
 (
  cd_demo_sk = ss_cdemo_sk
  and
   cd_marital_status = 'S'
   and
   cd_education_status = 'College'
   and
   ss_sales_price between 150.00::numeric and 200.00::numeric
 )
 )
 and
 (
  (
  ss_addr_sk = ca_address_sk
  and
  ca_country = 'United States'
  and
  ca_state in ('CO', 'OH', 'TX')
  and ss_net_profit between 0::numeric and 2000::numeric
  )
 or
  (ss_addr_sk = ca_address_sk
  and
  ca_country = 'United States'
  and
  ca_state in ('OR', 'MN', 'KY')
  and ss_net_profit between 150::numeric and 3000::numeric
  )
 or
  (ss_addr_sk = ca_address_sk
  and
  ca_country = 'United States'
  and
  ca_state in ('VA', 'CA', 'MS')
  and ss_net_profit between 50::numeric and 25000::numeric
  )
 )
;

-- end query 1 in stream 0 using template ../query_templates/query48.tpl
